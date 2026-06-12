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
#include <sys/uio.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/btrfs_structures.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "mem_tracker.h"
#include "migration_map.h"
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

struct block_range {
  uint64_t start;
  uint64_t count;
};

#define RELOC_MAX_CHUNK (16U * 1024U * 1024U)

static int cmp_u64(const void *a, const void *b) {
  uint64_t va = *(const uint64_t *)a;
  uint64_t vb = *(const uint64_t *)b;
  if (va < vb)
    return -1;
  if (va > vb)
    return 1;
  return 0;
}

static int cmp_block_range(const void *a, const void *b) {
  const struct block_range *ra = a;
  const struct block_range *rb = b;
  if (ra->start < rb->start)
    return -1;
  if (ra->start > rb->start)
    return 1;
  return 0;
}

static int block_ranges_append(struct block_range **ranges, uint32_t *count,
                               uint32_t *capacity, uint64_t start,
                               uint64_t nblocks) {
  if (nblocks == 0)
    return 0;

  if (*count >= *capacity) {
    uint32_t new_cap = *capacity ? *capacity * 2 : 64;
    struct block_range *nr =
        realloc(*ranges, new_cap * sizeof(struct block_range));
    if (!nr)
      return -1;
    *ranges = nr;
    *capacity = new_cap;
  }

  (*ranges)[*count].start = start;
  (*ranges)[*count].count = nblocks;
  (*count)++;
  return 0;
}

static void block_ranges_merge(struct block_range *ranges, uint32_t *count) {
  if (*count <= 1)
    return;

  qsort(ranges, *count, sizeof(struct block_range), cmp_block_range);

  uint32_t out = 0;
  for (uint32_t i = 1; i < *count; i++) {
    struct block_range *prev = &ranges[out];
    struct block_range *curr = &ranges[i];
    uint64_t prev_end = prev->start + prev->count;

    if (curr->start <= prev_end) {
      uint64_t curr_end = curr->start + curr->count;
      if (curr_end > prev_end)
        prev->count = curr_end - prev->start;
    } else {
      out++;
      ranges[out] = *curr;
    }
  }
  *count = out + 1;
}

static void mark_block_range(uint8_t *bitmap, uint64_t start, uint64_t count) {
  for (uint64_t i = 0; i < count; i++) {
    uint64_t b = start + i;
    bitmap[b / 8] |= (1 << (b % 8));
  }
}

static int free_space_init(struct free_space *fs,
                           const struct ext4_layout *layout,
                           const struct btrfs_fs_info *fs_info) {
  uint32_t block_size = layout->block_size;
  uint64_t total_blocks = layout->total_blocks;

  memset(fs, 0, sizeof(*fs));
  fs->total_blocks = total_blocks;

  fs->bitmap = calloc((total_blocks + 7) / 8, 1);
  if (!fs->bitmap)
    return -1;

  struct block_range *ranges = NULL;
  uint32_t range_count = 0;
  uint32_t range_cap = 0;

  /* Reserved blocks: sort and merge consecutive runs */
  if (layout->reserved_block_count > 0) {
    uint64_t *sorted =
        malloc((size_t)layout->reserved_block_count * sizeof(uint64_t));
    if (!sorted)
      goto fail;

    for (uint32_t i = 0; i < layout->reserved_block_count; i++)
      sorted[i] = layout->reserved_blocks[i];

    qsort(sorted, layout->reserved_block_count, sizeof(uint64_t), cmp_u64);

    uint64_t run_start = sorted[0];
    uint64_t run_len = 1;
    for (uint32_t i = 1; i <= layout->reserved_block_count; i++) {
      if (i < layout->reserved_block_count &&
          sorted[i] == sorted[i - 1] + 1) {
        run_len++;
      } else {
        if (run_start < total_blocks) {
          uint64_t clip = run_len;
          if (run_start + clip > total_blocks)
            clip = total_blocks - run_start;
          if (block_ranges_append(&ranges, &range_count, &range_cap, run_start,
                                  clip) < 0) {
            free(sorted);
            goto fail;
          }
        }
        if (i < layout->reserved_block_count) {
          run_start = sorted[i];
          run_len = 1;
        }
      }
    }
    free(sorted);
  }

  /* Btrfs data: prefer extent-tree ranges from fs_info (O(extents)). */
  if (fs_info->used_blocks.count > 0) {
    for (uint32_t i = 0; i < fs_info->used_blocks.count; i++) {
      const struct used_extent *ue = &fs_info->used_blocks.extents[i];
      if (!(ue->flags & BTRFS_BLOCK_GROUP_DATA))
        continue;

      uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ue->start);
      if (phys == (uint64_t)-1)
        continue;

      uint64_t start_block = phys / block_size;
      uint64_t num_blocks = (ue->length + block_size - 1) / block_size;
      if (start_block >= total_blocks)
        continue;
      if (start_block + num_blocks > total_blocks)
        num_blocks = total_blocks - start_block;

      if (block_ranges_append(&ranges, &range_count, &range_cap, start_block,
                              num_blocks) < 0)
        goto fail;
    }
  } else {
    /* Fallback: FS-tree file extents (skip inline/PREALLOC/empty) */
    for (uint32_t i = 0; i < fs_info->inode_count; i++) {
      const struct file_entry *fe = fs_info->inode_table[i];
      for (uint32_t j = 0; j < fe->extent_count; j++) {
        const struct file_extent *ext = &fe->extents[j];
        if (ext->type == BTRFS_FILE_EXTENT_INLINE ||
            ext->type == BTRFS_FILE_EXTENT_PREALLOC || ext->disk_bytenr == 0)
          continue;

        uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
        if (phys == (uint64_t)-1)
          continue;

        uint64_t start_block = phys / block_size;
        uint64_t num_blocks =
            (ext->disk_num_bytes + block_size - 1) / block_size;
        if (start_block >= total_blocks)
          continue;
        if (start_block + num_blocks > total_blocks)
          num_blocks = total_blocks - start_block;

        if (block_ranges_append(&ranges, &range_count, &range_cap, start_block,
                                num_blocks) < 0)
          goto fail;
      }
    }
  }

  block_ranges_merge(ranges, &range_count);

  uint64_t used_blocks = 0;
  for (uint32_t i = 0; i < range_count; i++) {
    mark_block_range(fs->bitmap, ranges[i].start, ranges[i].count);
    used_blocks += ranges[i].count;
  }
  free(ranges);

  fs->free_count = total_blocks - used_blocks;
  printf("  Free blocks available: %lu\n", (unsigned long)fs->free_count);
  return 0;

fail:
  free(ranges);
  free(fs->bitmap);
  memset(fs, 0, sizeof(*fs));
  return -1;
}

static inline int fs_is_used(const uint8_t *bitmap, uint64_t block) {
  return (bitmap[block / 8] >> (block % 8)) & 1;
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
      if (ext->type == BTRFS_FILE_EXTENT_INLINE ||
          ext->type == BTRFS_FILE_EXTENT_PREALLOC || ext->disk_bytenr == 0)
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
      if (ext->type == BTRFS_FILE_EXTENT_INLINE ||
          ext->type == BTRFS_FILE_EXTENT_PREALLOC || ext->disk_bytenr == 0)
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
 * Relocation executor — io_uring batch I/O for contiguous runs
 * ======================================================================== */

static int entries_are_adjacent(const struct relocation_entry *a,
                                const struct relocation_entry *b) {
  return a->src_offset + a->length == b->src_offset &&
         a->dst_offset + a->length == b->dst_offset;
}

static void update_entry_extents(struct relocation_entry *re,
                                 struct btrfs_fs_info *fs_info,
                                 struct extent_hash *ehash, int have_hash,
                                 uint32_t block_size) {
  uint32_t blocks_in_entry = (uint32_t)(re->length / block_size);
  for (uint32_t bi = 0; bi < blocks_in_entry; bi++) {
    uint64_t src_block_offset = re->src_offset + (uint64_t)bi * block_size;

    if (have_hash) {
      uint32_t slot =
          (uint32_t)((src_block_offset * 2654435761ULL) >> 16) % ehash->size;
      uint32_t start = slot;
      int first_extent = 1;

      do {
        if (ehash->buckets[slot].phys_offset == 0)
          break;

        if (ehash->buckets[slot].phys_offset == src_block_offset) {
          uint32_t fi = ehash->buckets[slot].inode_idx;
          uint32_t ej = ehash->buckets[slot].extent_idx;
          if (fi < fs_info->inode_count &&
              ej < fs_info->inode_table[fi]->extent_count) {
            if (first_extent) {
              fs_info->inode_table[fi]->extents[ej].disk_bytenr =
                  re->dst_offset + (uint64_t)bi * block_size;
              first_extent = 0;
            } else {
              fs_info->inode_table[fi]->extents[ej].disk_bytenr =
                  re->dst_offset + (uint64_t)bi * block_size;
            }
          }
        }
        slot = (slot + 1) % ehash->size;
      } while (slot != start);
    } else {
      for (uint32_t fi = 0; fi < fs_info->inode_count; fi++) {
        struct file_entry *fe = fs_info->inode_table[fi];
        for (uint32_t ej = 0; ej < fe->extent_count; ej++) {
          struct file_extent *ext = &fe->extents[ej];
          if (ext->type == BTRFS_FILE_EXTENT_INLINE ||
              ext->type == BTRFS_FILE_EXTENT_PREALLOC || ext->disk_bytenr == 0)
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
}

/*
 * Transfer a coalesced group of adjacent entries.
 * Multi-block contiguous chunks use device_batch_* (io_uring); larger runs
 * or single-block slices fall back to pread/pwrite.
 */
static int relocate_group_batch(struct device *dev,
                                struct relocation_entry *entries,
                                uint32_t start_idx, uint32_t end_idx,
                                uint8_t *buf, size_t buf_cap,
                                uint32_t block_size) {
  uint64_t grp_src = entries[start_idx].src_offset;
  uint64_t grp_dst = entries[start_idx].dst_offset;
  uint64_t grp_total = 0;

  for (uint32_t e = start_idx; e <= end_idx; e++) {
    grp_total += entries[e].length;
    entries[e].checksum = 0;
  }

  uint64_t grp_off = 0;
  while (grp_off < grp_total) {
    uint64_t chunk = grp_total - grp_off;
    if (chunk > buf_cap)
      chunk = buf_cap;

    uint64_t blocks = chunk / block_size;
    int use_batch = (blocks > 1) && (blocks <= DEVICE_BATCH_QUEUE_DEPTH);

    if (use_batch) {
      if (device_batch_read(dev, grp_src + grp_off, buf, (size_t)chunk) < 0)
        return -1;
    } else if (device_read(dev, grp_src + grp_off, buf, (size_t)chunk) < 0) {
      return -1;
    }

    /* Per-entry checksum over this chunk slice */
    uint64_t chunk_start = grp_off;
    uint64_t chunk_end = grp_off + chunk;
    uint64_t entry_base = 0;
    for (uint32_t e = start_idx; e <= end_idx; e++) {
      uint64_t e_start = entry_base;
      uint64_t e_end = entry_base + entries[e].length;
      entry_base = e_end;

      if (e_end <= chunk_start || e_start >= chunk_end)
        continue;

      uint64_t slice_start =
          e_start > chunk_start ? e_start : chunk_start;
      uint64_t slice_end = e_end < chunk_end ? e_end : chunk_end;
      size_t slice_len = (size_t)(slice_end - slice_start);
      size_t buf_off = (size_t)(slice_start - grp_off);

      entries[e].checksum =
          crc32c(entries[e].checksum, buf + buf_off, slice_len);
    }

    if (use_batch) {
      if (device_batch_write(dev, grp_dst + grp_off, buf, (size_t)chunk) < 0)
        return -1;
    } else if (device_write(dev, grp_dst + grp_off, buf, (size_t)chunk) < 0) {
      return -1;
    }

    grp_off += chunk;
  }

  return 0;
}

int relocator_execute(struct relocation_plan *plan, struct device *dev,
                      struct btrfs_fs_info *fs_info, uint32_t block_size) {
  if (plan->count == 0) {
    printf("No blocks need relocation.\n\n");
    return 0;
  }

  uint32_t done0 = migration_map_completed_count(plan);
  printf("Executing %u block relocations (%u already completed)...\n",
         plan->count, done0);

  struct extent_hash ehash;
  int have_hash = (extent_hash_init(&ehash, fs_info, block_size) == 0);

  uint64_t max_len = RELOC_MAX_CHUNK;
  for (uint32_t i = 0; i < plan->count; i++) {
    if (plan->entries[i].length > max_len)
      max_len = plan->entries[i].length;
  }
  if (max_len > RELOC_MAX_CHUNK)
    max_len = RELOC_MAX_CHUNK;

  uint8_t *buf = malloc(max_len);
  if (!buf) {
    if (have_hash)
      extent_hash_free(&ehash);
    return -1;
  }

  if (device_read_batch_begin(dev) < 0) {
    free(buf);
    if (have_hash)
      extent_hash_free(&ehash);
    return -1;
  }

  for (uint32_t i = 0; i < plan->count;) {
    if (plan->entries[i].completed) {
      i++;
      continue;
    }

    uint32_t group_end = i;
    while (group_end + 1 < plan->count &&
           !plan->entries[group_end + 1].completed &&
           entries_are_adjacent(&plan->entries[group_end],
                                &plan->entries[group_end + 1])) {
      group_end++;
    }

    if (relocate_group_batch(dev, plan->entries, i, group_end, buf,
                             (size_t)max_len, block_size) < 0) {
      free(buf);
      if (have_hash)
        extent_hash_free(&ehash);
      fprintf(stderr,
              "btrfs2ext4: relocation I/O failed at seq %u\n",
              plan->entries[i].seq);
      return -1;
    }

    for (uint32_t e = i; e <= group_end; e++) {
      struct relocation_entry *re = &plan->entries[e];
      update_entry_extents(re, fs_info, &ehash, have_hash, block_size);

      re->completed = 1;
      if (migration_map_update_entry(dev, e, re) < 0) {
        free(buf);
        if (have_hash)
          extent_hash_free(&ehash);
        return -1;
      }

      uint32_t completed = migration_map_completed_count(plan);
      printf("Pass 2: entry=%u/%u completed=%u\n", e + 1, plan->count,
             completed);
    }

    i = group_end + 1;
  }

  free(buf);
  if (have_hash)
    extent_hash_free(&ehash);

  printf("  Block relocation complete\n\n");
  return 0;
}

void relocator_free(struct relocation_plan *plan) {
  free(plan->entries);
  memset(plan, 0, sizeof(*plan));
}
