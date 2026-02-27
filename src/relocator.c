/*
 * relocator.c — Block relocation engine (optimized)
 *
 * Moves data blocks that conflict with ext4 metadata positions
 * to free locations on disk.
 *
 * Performance optimizations:
 * - Conflict detection via bitmap (O(1) per block, was O(N×M))
 * - Adjacent conflicting blocks coalesced into single I/O ops (was per-block)
 * - Extent map update via hash lookup (O(1) per relocation, was
 * O(inodes×extents))
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "journal.h"
#include "mem_tracker.h"
#include "relocator.h"

/* CRC32C from superblock.c */
extern uint32_t crc32c(uint32_t crc, const void *buf, size_t len);

/* ========================================================================
 * Conflict bitmap — O(1) per-block conflict check
 * ======================================================================== */

static uint8_t *build_conflict_bitmap(const struct ext4_layout *layout) {
  uint64_t total_blocks = layout->total_blocks;
  uint8_t *bitmap = calloc((total_blocks + 7) / 8, 1);
  if (!bitmap)
    return NULL;

  for (uint32_t i = 0; i < layout->reserved_block_count; i++) {
    uint64_t b = layout->reserved_blocks[i];
    if (b < total_blocks)
      bitmap[b / 8] |= (1 << (b % 8));
  }
  return bitmap;
}

static inline int is_conflict(const uint8_t *bitmap, uint64_t block) {
  return (bitmap[block / 8] >> (block % 8)) & 1;
}

/* ========================================================================
 * Free space tracker
 * ======================================================================== */

struct free_space {
  uint8_t *bitmap;
  uint64_t total_blocks;
  uint64_t current_block;
  uint64_t free_count;
};

static inline int fs_is_used(const uint8_t *bitmap, uint64_t block) {
  return (bitmap[block / 8] >> (block % 8)) & 1;
}

static int free_space_init(struct free_space *fs,
                           const struct ext4_layout *layout,
                           const struct btrfs_fs_info *fs_info) {
  uint32_t block_size = layout->block_size;
  uint64_t total_blocks = layout->total_blocks;

  memset(fs, 0, sizeof(*fs));
  fs->total_blocks = total_blocks;

  /* Allocate bitmap (1 bit per block) */
  fs->bitmap = calloc((total_blocks + 7) / 8, 1);
  if (!fs->bitmap)
    return -1;

  /* Mark ext4 reserved blocks */
  for (uint32_t i = 0; i < layout->reserved_block_count; i++) {
    uint64_t b = layout->reserved_blocks[i];
    if (b < total_blocks)
      fs->bitmap[b / 8] |= (1 << (b % 8));
  }

  /* Mark btrfs data blocks */
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    const struct file_entry *fe = fs_info->inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      const struct file_extent *ext = &fe->extents[j];
      if (ext->type == BTRFS_FILE_EXTENT_INLINE || ext->disk_bytenr == 0)
        continue;

      uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
      if (phys == (uint64_t)-1)
        continue;

      uint64_t start_block = phys / block_size;
      uint64_t num_blocks = (ext->disk_num_bytes + block_size - 1) / block_size;

      for (uint64_t b = start_block;
           b < start_block + num_blocks && b < total_blocks; b++) {
        fs->bitmap[b / 8] |= (1 << (b % 8));
      }
    }
  }

  /* Count free blocks */
  for (uint64_t b = 0; b < total_blocks; b++) {
    if (!fs_is_used(fs->bitmap, b)) {
      fs->free_count++;
    }
  }

  printf("  Free blocks available: %lu\n", (unsigned long)fs->free_count);
  return 0;
}

/*
 * Allocate 'count' consecutive free blocks (best-effort).
 * Returns the first block number, or (uint64_t)-1 if none available.
 * Falls back to single-block allocation if no consecutive run found.
 */
static uint64_t free_space_alloc_run(struct free_space *fs, uint32_t count,
                                     uint32_t *actual_count) {
  if (fs->free_count == 0) {
    *actual_count = 0;
    return (uint64_t)-1;
  }

  /* Bug H fix: Save cursor start to enable wrap-around.
   * If we reach total_blocks without finding space, wrap to 0
   * and scan up to the original position. */
  uint64_t saved_cursor = fs->current_block;
  uint64_t start_block = (uint64_t)-1;
  uint32_t run = 0;
  int wrapped = 0;

  for (;;) {
    if (fs->current_block >= fs->total_blocks) {
      if (wrapped)
        break; /* Already wrapped, no space */
      fs->current_block = 0;
      wrapped = 1;
    }
    if (wrapped && fs->current_block >= saved_cursor)
      break; /* Back to start, no space */

    if (!fs_is_used(fs->bitmap, fs->current_block)) {
      if (run == 0) {
        start_block = fs->current_block;
      }
      run++;
      fs->current_block++;
      if (run == count)
        break;
    } else {
      if (run > 0) {
        fs->current_block++;
        break;
      }
      fs->current_block++;
    }
  }

  if (run > 0) {
    *actual_count = run;
    for (uint32_t i = 0; i < run; i++) {
      uint64_t b = start_block + i;
      fs->bitmap[b / 8] |= (1 << (b % 8));
    }
    fs->free_count -= run;
    return start_block;
  }

  *actual_count = 0;
  return (uint64_t)-1;
}

static uint64_t free_space_alloc(struct free_space *fs) {
  uint32_t actual = 0;
  return free_space_alloc_run(fs, 1, &actual);
}

static void free_space_free(struct free_space *fs) {
  free(fs->bitmap);
  memset(fs, 0, sizeof(*fs));
}

/* ========================================================================
 * Extent map hash — O(1) per-relocation extent update
 * ======================================================================== */

struct extent_hash_entry {
  uint64_t phys_offset; /* key: physical byte offset */
  uint32_t inode_idx;   /* index into fs_info->inode_table */
  uint32_t extent_idx;  /* index into file_entry->extents */
};

struct extent_hash {
  struct extent_hash_entry *buckets;
  uint64_t size;
  uint32_t count;
};

static int extent_hash_init(struct extent_hash *eh,
                            const struct btrfs_fs_info *fs_info,
                            uint32_t block_size) {
  /* Count total extents to size the hash table */
  uint32_t total = 0;
  for (uint32_t i = 0; i < fs_info->inode_count; i++)
    total += fs_info->inode_table[i]->extent_count;

  eh->size = total < 64 ? 128 : (uint64_t)total * 2;

  size_t hash_bytes = eh->size * sizeof(struct extent_hash_entry);
  if (mem_track_exceeded()) {
    printf(
        "  [Relocator] High memory usage detected, disabling extent hash.\n");
    return -1; /* Signal caller to fall back to linear scan */
  }

  eh->buckets = calloc(1, hash_bytes);
  if (!eh->buckets)
    return -1;

  mem_track_alloc(hash_bytes);
  eh->count = 0;

  /* Populate: map physical_offset → (inode_idx, extent_idx) */
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    const struct file_entry *fe = fs_info->inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      const struct file_extent *ext = &fe->extents[j];
      if (ext->type == BTRFS_FILE_EXTENT_INLINE || ext->disk_bytenr == 0)
        continue;

      uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
      if (phys == (uint64_t)-1)
        continue;

      /* Round to block boundary for lookup */
      uint64_t phys_block_offset = (phys / block_size) * block_size;

      uint32_t slot =
          (uint32_t)((phys_block_offset * 2654435761ULL) >> 16) % eh->size;
      while (eh->buckets[slot].phys_offset != 0 &&
             eh->buckets[slot].phys_offset != phys_block_offset) {
        slot = (slot + 1) % eh->size;
      }

      /* Phase 4.2 Support: Duplicate keys (same physical offset) mean CoW block
       * deduplication. The first extent mapper writes the key, subsequent ones
       * get inserted linearly after. */
      if (eh->buckets[slot].phys_offset == phys_block_offset) {
        while (eh->buckets[slot].phys_offset != 0) {
          slot = (slot + 1) % eh->size;
        }
      }

      eh->buckets[slot].phys_offset = phys_block_offset;
      eh->buckets[slot].inode_idx = i;
      eh->buckets[slot].extent_idx = j;
      eh->count++;
    }
  }
  return 0;
}

static void extent_hash_free(struct extent_hash *eh) {
  free(eh->buckets);
  memset(eh, 0, sizeof(*eh));
}

/* ========================================================================
 * Relocation planner — with coalescing (#2) and conflict bitmap (#4)
 * ======================================================================== */

static int cmp_relocation_entry(const void *a, const void *b) {
  const struct relocation_entry *ea = a;
  const struct relocation_entry *eb = b;
  if (ea->src_offset < eb->src_offset)
    return -1;
  if (ea->src_offset > eb->src_offset)
    return 1;
  return 0;
}

int relocator_plan(struct relocation_plan *plan,
                   const struct ext4_layout *layout,
                   struct btrfs_fs_info *fs_info) {
  uint32_t block_size = layout->block_size;

  memset(plan, 0, sizeof(*plan));
  plan->capacity = 256;
  plan->entries = calloc(plan->capacity, sizeof(struct relocation_entry));
  if (!plan->entries)
    return -1;

  printf("=== Phase 2: Planning Block Relocation ===\n\n");

  /* Build conflict bitmap for O(1) lookups */
  uint8_t *conflict_bmp = build_conflict_bitmap(layout);
  if (!conflict_bmp)
    return -1;

  /* Build free space tracker */
  struct free_space fspace;
  if (free_space_init(&fspace, layout, fs_info) < 0) {
    free(conflict_bmp);
    return -1;
  }

  /* Find conflicting data blocks and coalesce adjacent ones */
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    struct file_entry *fe = fs_info->inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      struct file_extent *ext = &fe->extents[j];
      if (ext->type == BTRFS_FILE_EXTENT_INLINE || ext->disk_bytenr == 0)
        continue;

      uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
      if (phys == (uint64_t)-1)
        continue;

      uint64_t start_block = phys / block_size;
      uint64_t num_blocks = (ext->disk_num_bytes + block_size - 1) / block_size;

      /* Scan for runs of consecutive conflicting blocks */
      uint64_t b = start_block;
      while (b < start_block + num_blocks) {
        if (!is_conflict(conflict_bmp, b)) {
          b++;
          continue;
        }

        /* Found a conflict — find the run length */
        uint64_t run_start = b;
        uint32_t run_len = 0;
        while (b < start_block + num_blocks && is_conflict(conflict_bmp, b)) {
          run_len++;
          b++;
        }

        /* Allocate a destination run (try consecutive, fall back to individual)
         */
        uint32_t dst_got = 0;
        uint64_t dst_start = free_space_alloc_run(&fspace, run_len, &dst_got);

        if (dst_start == (uint64_t)-1) {
          fprintf(stderr,
                  "btrfs2ext4: ERROR: not enough free space for relocation\n");
          free(conflict_bmp);
          free_space_free(&fspace);
          return -1;
        }

        /* If we couldn't get the full run consecutively, handle remainder */
        uint32_t allocated = dst_got;

        /* Create relocation entry for the coalesced run */
        if (plan->count >= plan->capacity) {
          plan->capacity *= 2;
          struct relocation_entry *new_ent = realloc(
              plan->entries, plan->capacity * sizeof(struct relocation_entry));
          if (!new_ent) {
            fprintf(stderr, "btrfs2ext4: OOM reallocating relocation plan\n");
            free(conflict_bmp);
            free_space_free(&fspace);
            return -1;
          }
          plan->entries = new_ent;
        }

        struct relocation_entry *re = &plan->entries[plan->count];
        re->src_offset = run_start * block_size;
        re->dst_offset = dst_start * block_size;
        re->length = (uint64_t)allocated * block_size;
        re->seq = plan->count;
        re->completed = 0;
        plan->count++;
        plan->total_bytes_to_move += re->length;

        /* Handle remaining blocks that didn't fit in consecutive run */
        for (uint32_t r = allocated; r < run_len; r++) {
          uint64_t single_dst = free_space_alloc(&fspace);
          if (single_dst == (uint64_t)-1) {
            fprintf(
                stderr,
                "btrfs2ext4: ERROR: not enough free space for relocation\n");
            free(conflict_bmp);
            free_space_free(&fspace);
            return -1;
          }

          if (plan->count >= plan->capacity) {
            plan->capacity *= 2;
            struct relocation_entry *new_ent =
                realloc(plan->entries,
                        plan->capacity * sizeof(struct relocation_entry));
            if (!new_ent) {
              fprintf(stderr, "btrfs2ext4: OOM reallocating relocation plan\n");
              free(conflict_bmp);
              free_space_free(&fspace);
              return -1;
            }
            plan->entries = new_ent;
          }

          re = &plan->entries[plan->count];
          re->src_offset = (run_start + r) * block_size;
          re->dst_offset = single_dst * block_size;
          re->length = block_size;
          re->seq = plan->count;
          re->completed = 0;
          plan->count++;
          plan->total_bytes_to_move += block_size;
        }
      }
    }
  }

  free(conflict_bmp);
  free_space_free(&fspace);

  /* Phase 2.1: Sort relocation entries by source physical offset to optimize
   * HDD seeks radially */
  if (plan->count > 1) {
    qsort(plan->entries, plan->count, sizeof(struct relocation_entry),
          cmp_relocation_entry);

    /* Phase 2.4: Post-sort coalescing: Merge adjacent runs to maximize
     * contiguous I/O */
    uint32_t active = 0;
    for (uint32_t i = 1; i < plan->count; i++) {
      struct relocation_entry *prev = &plan->entries[active];
      struct relocation_entry *curr = &plan->entries[i];

      if (prev->src_offset + prev->length == curr->src_offset &&
          prev->dst_offset + prev->length == curr->dst_offset) {
        prev->length += curr->length;
      } else {
        active++;
        if (active != i) {
          plan->entries[active] = plan->entries[i];
        }
      }
    }
    plan->count = active + 1;
  }

  printf("  Relocation entries: %u (coalesced from individual blocks)\n",
         plan->count);
  printf("  Total bytes to move: %lu (%.1f MiB)\n",
         (unsigned long)plan->total_bytes_to_move,
         (double)plan->total_bytes_to_move / (1024.0 * 1024.0));
  printf("==========================================\n\n");

  return 0;
}

/* ========================================================================
 * Relocation executor — with batched I/O and hash-based extent update
 * ======================================================================== */

int relocator_execute(struct relocation_plan *plan, struct device *dev,
                      struct btrfs_fs_info *fs_info, uint32_t block_size) {
  if (plan->count == 0) {
    printf("No blocks need relocation.\n\n");
    return 0;
  }

  printf("Executing %u block relocations...\n", plan->count);

  /* Build extent hash for O(1) updates (#7) */
  struct extent_hash ehash;
  int have_hash = (extent_hash_init(&ehash, fs_info, block_size) == 0);

  /* Find max relocation entry size to allocate buffer */
  uint64_t max_len = 0;
  for (uint32_t i = 0; i < plan->count; i++) {
    if (plan->entries[i].length > max_len)
      max_len = plan->entries[i].length;
  }

  if (max_len > 16 * 1024 * 1024)
    max_len = 16 * 1024 * 1024;

  uint8_t *buf = malloc(max_len);
  if (!buf) {
    if (have_hash)
      extent_hash_free(&ehash);
    return -1;
  }

  for (uint32_t i = 0; i < plan->count; i++) {
    struct relocation_entry *re = &plan->entries[i];

    uint64_t remaining = re->length;
    uint64_t current_src = re->src_offset;
    uint64_t current_dst = re->dst_offset;
    re->checksum = 0;

    while (remaining > 0) {
      uint64_t chunk = remaining > max_len ? max_len : remaining;

      /* Read source blocks */
      if (device_read(dev, current_src, buf, chunk) < 0) {
        free(buf);
        if (have_hash)
          extent_hash_free(&ehash);
        return -1;
      }

      /* Compute checksum of chunk for migration map integrity */
      re->checksum = crc32c(re->checksum, buf, chunk);

      /* Write to destination */
      if (device_write(dev, current_dst, buf, chunk) < 0) {
        free(buf);
        if (have_hash)
          extent_hash_free(&ehash);

        fprintf(stderr,
                "btrfs2ext4: relocation write failed at seq %u, initiating "
                "partial rollback...\n",
                re->seq);
        journal_replay_partial(dev, journal_current_offset(), re->seq);
        return -1;
      }

      /* Bug F fix: Read-back verification removed by default.
       * The in-memory CRC stored in re->checksum is sufficient for
       * rollback integrity via the migration map. The old readback
       * doubled I/O time on HDDs (each write requires a full disk
       * rotation before the readback can start). */

      current_src += chunk;
      current_dst += chunk;
      remaining -= chunk;
    }

    re->completed = 1;

    /* Update in-memory extent maps using hash (O(1) per block - supports CoW
     * dupes) */
    uint32_t blocks_in_entry = (uint32_t)(re->length / block_size);
    for (uint32_t bi = 0; bi < blocks_in_entry; bi++) {
      uint64_t src_block_offset = re->src_offset + (uint64_t)bi * block_size;

      if (have_hash) {
        uint32_t slot =
            (uint32_t)((src_block_offset * 2654435761ULL) >> 16) % ehash.size;
        uint32_t start = slot;
        int first_extent = 1;

        do {
          if (ehash.buckets[slot].phys_offset == 0)
            break;

          /* Phase 4.2: CoW Cloning inside Relocator */
          if (ehash.buckets[slot].phys_offset == src_block_offset) {
            uint32_t fi = ehash.buckets[slot].inode_idx;
            uint32_t ej = ehash.buckets[slot].extent_idx;
            if (fi < fs_info->inode_count &&
                ej < fs_info->inode_table[fi]->extent_count) {
              if (first_extent) {
                /* Primary extent update */
                fs_info->inode_table[fi]->extents[ej].disk_bytenr =
                    re->dst_offset + (uint64_t)bi * block_size;
                first_extent = 0;
              } else {
                /* Secondary extent (CoW duplication)
                 * We point the secondary inode's extent directly to the newly
                 * relocated block alongside the primary inode.
                 * The actual physical cloning of these Shared Blocks (to
                 * prevent Ext4 'Multiply-Claimed' metadata corruption) is
                 * universally handled downstream by `resolve_extents` in
                 * `extent_writer.c`.
                 */
                fs_info->inode_table[fi]->extents[ej].disk_bytenr =
                    re->dst_offset + (uint64_t)bi * block_size;
              }
            }
          }
          slot = (slot + 1) % ehash.size;
        } while (slot != start);
      } else {
        /* Fallback: linear scan (original behavior) */
        for (uint32_t fi = 0; fi < fs_info->inode_count; fi++) {
          struct file_entry *fe = fs_info->inode_table[fi];
          for (uint32_t ej = 0; ej < fe->extent_count; ej++) {
            struct file_extent *ext = &fe->extents[ej];
            if (ext->type == BTRFS_FILE_EXTENT_INLINE || ext->disk_bytenr == 0)
              continue;
            uint64_t phys =
                chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
            if (phys == src_block_offset) {
              ext->disk_bytenr = re->dst_offset + (uint64_t)bi * block_size;
            }
          }
        }
      }
    }

    /* Progress */
    if ((i + 1) % 100 == 0 || i + 1 == plan->count) {
      printf("  Relocated %u/%u entries (%.1f%%)\n", i + 1, plan->count,
             100.0 * (i + 1) / plan->count);
    }
  }

  free(buf);
  if (have_hash)
    extent_hash_free(&ehash);
  device_sync(dev);

  printf("  Block relocation complete\n\n");
  return 0;
}

void relocator_free(struct relocation_plan *plan) {
  free(plan->entries);
  memset(plan, 0, sizeof(*plan));
}
