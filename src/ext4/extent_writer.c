/*
 * extent_writer.c — Ext4 extent tree writer
 *
 * Builds ext4 extent trees from btrfs extent data.
 * Supports multi-level extent trees for files with many extents.
 *
 * Ext4 extent tree structure:
 * - Depth 0 (leaf): extent header + up to N ext4_extent entries
 * - Depth 1+: extent header + ext4_extent_idx entries pointing to child blocks
 *
 * The root of the tree is stored inline in the inode's i_block (60 bytes):
 *   - 12 bytes for ext4_extent_header
 *   - 48 bytes = 4 * 12 bytes for ext4_extent or ext4_extent_idx
 *
 * External extent blocks (4096 bytes):
 *   - 12 bytes header
 *   - Remaining for entries: (4096-12)/12 = 340 entries
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

/* Maximum extents in an inline (inode) extent tree */
#define INLINE_EXTENT_MAX 4

/* Maximum extents per external block: (block_size - header) / sizeof(extent) */
#define EXTENTS_PER_BLOCK(bs)                                                  \
  (((bs) - sizeof(struct ext4_extent_header)) / sizeof(struct ext4_extent))

/* Maximum index entries per external block */
#define INDEX_PER_BLOCK(bs)                                                    \
  (((bs) - sizeof(struct ext4_extent_header)) / sizeof(struct ext4_extent_idx))

/* ========================================================================
 * Block allocator (bitmap-based, O(1) per allocation)
 * ======================================================================== */

void ext4_block_alloc_init(struct ext4_block_allocator *alloc,
                           const struct ext4_layout *layout) {
  memset(alloc, 0, sizeof(*alloc));

  /* Start allocating near the end of the data area to reduce early clashes. */
  if (layout->num_groups > 0) {
    uint32_t last = layout->num_groups - 1;
    alloc->next_alloc_block = layout->groups[last].data_start_block +
                              layout->groups[last].data_blocks;
    uint64_t reserve;
    if (layout->total_blocks > 10240) {
      reserve = layout->total_blocks / 10;
      if (reserve > 262144) /* cap at ~1GB for huge disks */
        reserve = 262144;
    } else {
      reserve = layout->total_blocks / 2;
    }

    if (alloc->next_alloc_block > reserve)
      alloc->next_alloc_block -= reserve;
    else
      alloc->next_alloc_block = 0;
  }
  alloc->max_blocks = layout->total_blocks;

  /* Build reserved bitmap for O(1) conflict checks and global usage map. */
  alloc->reserved_bitmap = calloc((layout->total_blocks + 7) / 8, 1);
  if (alloc->reserved_bitmap) {
    /* Seed with all metadata blocks reserved in the layout. */
    for (uint32_t i = 0; i < layout->reserved_block_count; i++) {
      uint64_t b = layout->reserved_blocks[i];
      if (b < layout->total_blocks)
        alloc->reserved_bitmap[b / 8] |= (1 << (b % 8));
    }
  }
}

void ext4_block_alloc_free(struct ext4_block_allocator *alloc) {
  free(alloc->reserved_bitmap);
  alloc->reserved_bitmap = NULL;
}

uint64_t ext4_alloc_block(struct ext4_block_allocator *alloc,
                          const struct ext4_layout *layout) {
  /* Sequential allocator with O(1) bitmap check per block */
  for (uint32_t g = 0; g < layout->num_groups; g++) {
    const struct ext4_bg_layout *bg = &layout->groups[g];
    for (uint32_t i = 0; i < bg->data_blocks; i++) {
      uint64_t block = bg->data_start_block + i;
      if (block <= alloc->next_alloc_block)
        continue;

      /* O(1) bitmap check: skip any block ya marcado como usado. */
      if (alloc->reserved_bitmap &&
          (alloc->reserved_bitmap[block / 8] & (1 << (block % 8)))) {
        continue;
      }

      /* Claim the block in the global usage bitmap. */
      if (alloc->reserved_bitmap)
        alloc->reserved_bitmap[block / 8] |= (1 << (block % 8));

      alloc->next_alloc_block = block;
      return block;
    }
  }
  return (uint64_t)-1; /* No free blocks */
}

void ext4_block_alloc_mark_fs_data(struct ext4_block_allocator *alloc,
                                   const struct ext4_layout *layout,
                                   const struct btrfs_fs_info *fs_info) {
  if (!alloc || !alloc->reserved_bitmap || !fs_info)
    return;

  uint32_t block_size = layout->block_size;

  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    const struct file_entry *fe = fs_info->inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      const struct file_extent *ext = &fe->extents[j];
      if (ext->type == BTRFS_FILE_EXTENT_INLINE || ext->disk_bytenr == 0)
        continue;

      /* ext->disk_bytenr puede ser lógico Btrfs (pre-reloc) o físico
       * (tras relocación u otras transformaciones). Intentar resolver
       * vía chunk_map y, si falla, tratarlo como físico directo. */
      uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
      if (phys == (uint64_t)-1)
        phys = ext->disk_bytenr;

      uint64_t start_blk = phys / block_size;
      uint64_t end_blk =
          (phys + ext->disk_num_bytes + block_size - 1) / block_size;

      for (uint64_t b = start_blk; b < end_blk && b < layout->total_blocks;
           b++) {
        alloc->reserved_bitmap[b / 8] |= (1 << (b % 8));
      }
    }
  }
}

/* ========================================================================
 * Internal: rebuild sorted extent list from btrfs data
 * ======================================================================== */

struct resolved_extent {
  uint32_t file_block; /* logical file block */
  uint32_t num_blocks; /* number of blocks */
  uint64_t phys_block; /* physical block on disk */
};

static int cmp_resolved_extent(const void *a, const void *b) {
  const struct resolved_extent *ea = (const struct resolved_extent *)a;
  const struct resolved_extent *eb = (const struct resolved_extent *)b;
  if (ea->file_block < eb->file_block)
    return -1;
  if (ea->file_block > eb->file_block)
    return 1;
  return 0;
}

/*
 * Build a sorted list of resolved extents from a btrfs file entry.
 * Physically clones CoW blocks to avoid Ext4 multiply-claimed metadata
 * corruption. Returns the number of extents, or -1 on error.
 */
static int resolve_extents(struct ext4_block_allocator *alloc,
                           struct device *dev, const struct ext4_layout *layout,
                           const struct file_entry *fe,
                           const struct chunk_map *chunk_map,
                           uint32_t block_size,
                           struct resolved_extent **out_extents) {
  if (fe->extent_count == 0) {
    *out_extents = NULL;
    return 0;
  }

  uint32_t capacity = fe->extent_count > 16 ? fe->extent_count : 16;
  struct resolved_extent *exts = calloc(capacity, sizeof(*exts));
  if (!exts)
    return -1;

  uint32_t count = 0;
  for (uint32_t i = 0; i < fe->extent_count; i++) {
    const struct file_extent *bext = &fe->extents[i];
    if (bext->type == BTRFS_FILE_EXTENT_INLINE || bext->disk_bytenr == 0)
      continue;

    uint64_t phys = chunk_map_resolve(chunk_map, bext->disk_bytenr);
    if (phys == (uint64_t)-1)
      continue;

    uint32_t file_block_start = (uint32_t)(bext->file_offset / block_size);
    uint32_t num_blocks = (uint32_t)(bext->num_bytes / block_size);
    uint64_t phys_block_start = phys / block_size;

    if (num_blocks == 0)
      num_blocks = 1;

    for (uint32_t b = 0; b < num_blocks; b++) {
      uint64_t current_phys = phys_block_start + b;
      uint32_t current_file_block = file_block_start + b;
      uint64_t final_phys = current_phys;

      if (alloc->reserved_bitmap && (alloc->reserved_bitmap[current_phys / 8] &
                                     (1 << (current_phys % 8)))) {
        /* Block already claimed! Physically clone it to avoid Ext4 CoW
         * conflicts */
        uint64_t new_phys = ext4_alloc_block(alloc, layout);
        if (new_phys != (uint64_t)-1) {
          if (alloc->reserved_bitmap) {
            alloc->reserved_bitmap[new_phys / 8] |= (1 << (new_phys % 8));
          }
          /* Copy data */
          uint8_t *tmp_buf = malloc(block_size);
          if (tmp_buf) {
            if (device_read(dev, current_phys * block_size, tmp_buf,
                            block_size) == 0) {
              device_write(dev, new_phys * block_size, tmp_buf, block_size);
            }
            free(tmp_buf);
          }
          final_phys = new_phys;
        }
      } else {
        /* Claim the physical block for this inode */
        if (alloc->reserved_bitmap) {
          alloc->reserved_bitmap[current_phys / 8] |= (1 << (current_phys % 8));
        }
      }

      if (count >= capacity) {
        capacity = capacity ? capacity * 2 : 16;
        struct resolved_extent *new_exts =
            realloc(exts, capacity * sizeof(*exts));
        if (!new_exts) {
          free(exts);
          return -1;
        }
        exts = new_exts;
      }

      exts[count].file_block = current_file_block;
      exts[count].num_blocks = 1;
      exts[count].phys_block = final_phys;
      count++;
    }
  }

  /* Sort by file block */
  if (count > 1)
    qsort(exts, count, sizeof(*exts), cmp_resolved_extent);

  /* Merge adjacent extents and enforce Ext4 limit (32768 blocks per extent) */
  uint32_t merged = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (merged > 0 &&
        exts[merged - 1].file_block + exts[merged - 1].num_blocks ==
            exts[i].file_block &&
        exts[merged - 1].phys_block + exts[merged - 1].num_blocks ==
            exts[i].phys_block &&
        exts[merged - 1].num_blocks + exts[i].num_blocks <= 32768) {
      exts[merged - 1].num_blocks += exts[i].num_blocks;
    } else {
      exts[merged++] = exts[i];
    }
  }

  *out_extents = exts;
  return (int)merged;
}

/* ========================================================================
 * Public: build extent tree (inline or multi-level)
 * ======================================================================== */

int ext4_build_extent_tree(struct ext4_block_allocator *alloc,
                           struct device *dev, struct ext4_inode *inode,
                           const struct file_entry *fe,
                           const struct chunk_map *chunk_map,
                           const struct ext4_layout *layout) {
  uint32_t block_size = layout->block_size;

  /* Resolve and merge all extents */
  struct resolved_extent *exts;
  int ext_count =
      resolve_extents(alloc, dev, layout, fe, chunk_map, block_size, &exts);
  if (ext_count < 0)
    return -1;
  if (ext_count == 0) {
    /* Empty file: write header only */
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    eh->eh_magic = htole16(EXT4_EXT_MAGIC);
    eh->eh_entries = htole16(0);
    eh->eh_max = htole16(INLINE_EXTENT_MAX);
    eh->eh_depth = htole16(0);
    eh->eh_generation = htole32(0);
    inode->i_flags |= htole32(EXT4_EXTENTS_FL);
    return 0;
  }

  if ((uint32_t)ext_count <= INLINE_EXTENT_MAX) {
    /* ================================================================
     * Simple case: all extents fit inline in inode
     * ================================================================ */
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    eh->eh_magic = htole16(EXT4_EXT_MAGIC);
    eh->eh_entries = htole16((uint16_t)ext_count);
    eh->eh_max = htole16(INLINE_EXTENT_MAX);
    eh->eh_depth = htole16(0);
    eh->eh_generation = htole32(0);

    struct ext4_extent *ext =
        (struct ext4_extent *)((uint8_t *)inode->i_block +
                               sizeof(struct ext4_extent_header));

    for (int i = 0; i < ext_count; i++) {
      ext[i].ee_block = htole32(exts[i].file_block);
      ext[i].ee_len = htole16((uint16_t)exts[i].num_blocks);
      ext[i].ee_start_lo = htole32((uint32_t)(exts[i].phys_block & 0xFFFFFFFF));
      ext[i].ee_start_hi = htole16((uint16_t)(exts[i].phys_block >> 32));
    }
  } else {
    /* ================================================================
     * General multi-level extent tree (arbitrary depth).
     *
     * Algorithm (bottom-up):
     *   1. Build depth-0 leaf blocks holding ext4_extent entries
     *   2. While the current level has more nodes than INLINE_EXTENT_MAX,
     *      build the next level of index blocks (ext4_extent_idx entries)
     *   3. Write the final top-level pointers into the inode root
     *
     * Capacity per depth (4 KiB blocks, EPB = IPB = 340):
     *   depth 0 (inline):  4 extents
     *   depth 1:           4 × 340             =     1,360
     *   depth 2:           4 × 340 × 340       =   462,400
     *   depth 3:           4 × 340 × 340 × 340 = 157,216,000
     * ================================================================ */
    uint32_t epb = (uint32_t)EXTENTS_PER_BLOCK(block_size);
    uint32_t ipb = (uint32_t)INDEX_PER_BLOCK(block_size);
    uint32_t num_leaves = ((uint32_t)ext_count + epb - 1) / epb;

    /*
     * Each tree node we've written is tracked as a (block_num,
     * first_file_block) pair so the parent level can build index entries
     * pointing to it.
     */
    struct tree_node {
      uint64_t block_num;
      uint32_t first_file_block;
    };

    struct tree_node *current_level =
        malloc(num_leaves * sizeof(*current_level));
    if (!current_level) {
      free(exts);
      return -1;
    }
    uint32_t current_count = num_leaves;
    uint16_t depth = 0; /* depth written so far */

    /* --- Step 1: write depth-0 leaf blocks --- */
    for (uint32_t leaf = 0; leaf < num_leaves; leaf++) {
      uint64_t blk = ext4_alloc_block(alloc, layout);
      if (blk == (uint64_t)-1) {
        fprintf(stderr, "btrfs2ext4: no space for extent tree leaf\n");
        free(current_level);
        free(exts);
        return -1;
      }

      uint32_t start_idx = leaf * epb;
      uint32_t leaf_count = (uint32_t)ext_count - start_idx;
      if (leaf_count > epb)
        leaf_count = epb;

      current_level[leaf].block_num = blk;
      current_level[leaf].first_file_block = exts[start_idx].file_block;

      uint8_t *leaf_buf = calloc(1, block_size);
      if (!leaf_buf) {
        free(current_level);
        free(exts);
        return -1;
      }

      struct ext4_extent_header *leh = (struct ext4_extent_header *)leaf_buf;
      leh->eh_magic = htole16(EXT4_EXT_MAGIC);
      leh->eh_entries = htole16((uint16_t)leaf_count);
      leh->eh_max = htole16((uint16_t)epb);
      leh->eh_depth = htole16(0);
      leh->eh_generation = htole32(0);

      struct ext4_extent *le =
          (struct ext4_extent *)(leaf_buf + sizeof(struct ext4_extent_header));
      for (uint32_t i = 0; i < leaf_count; i++) {
        uint32_t idx = start_idx + i;
        le[i].ee_block = htole32(exts[idx].file_block);
        le[i].ee_len = htole16((uint16_t)exts[idx].num_blocks);
        le[i].ee_start_lo =
            htole32((uint32_t)(exts[idx].phys_block & 0xFFFFFFFF));
        le[i].ee_start_hi = htole16((uint16_t)(exts[idx].phys_block >> 32));
      }

      if (device_write(dev, blk * block_size, leaf_buf, block_size) < 0) {
        free(leaf_buf);
        free(current_level);
        free(exts);
        return -1;
      }
      free(leaf_buf);
    }
    depth = 1; /* leaf level written → tree has at least depth 1 */

    /* --- Step 2: build index levels until we fit in the inode root --- */
    while (current_count > INLINE_EXTENT_MAX) {
      uint32_t num_idx = (current_count + ipb - 1) / ipb;

      struct tree_node *next_level = malloc(num_idx * sizeof(*next_level));
      if (!next_level) {
        free(current_level);
        free(exts);
        return -1;
      }

      for (uint32_t n = 0; n < num_idx; n++) {
        uint64_t blk = ext4_alloc_block(alloc, layout);
        if (blk == (uint64_t)-1) {
          fprintf(stderr, "btrfs2ext4: no space for extent tree index block\n");
          free(next_level);
          free(current_level);
          free(exts);
          return -1;
        }

        uint32_t start = n * ipb;
        uint32_t count = current_count - start;
        if (count > ipb)
          count = ipb;

        next_level[n].block_num = blk;
        next_level[n].first_file_block = current_level[start].first_file_block;

        uint8_t *idx_buf = calloc(1, block_size);
        if (!idx_buf) {
          free(next_level);
          free(current_level);
          free(exts);
          return -1;
        }

        struct ext4_extent_header *ih = (struct ext4_extent_header *)idx_buf;
        ih->eh_magic = htole16(EXT4_EXT_MAGIC);
        ih->eh_entries = htole16((uint16_t)count);
        ih->eh_max = htole16((uint16_t)ipb);
        ih->eh_depth = htole16(depth);
        ih->eh_generation = htole32(0);

        struct ext4_extent_idx *eidx =
            (struct ext4_extent_idx *)(idx_buf +
                                       sizeof(struct ext4_extent_header));
        for (uint32_t i = 0; i < count; i++) {
          eidx[i].ei_block = htole32(current_level[start + i].first_file_block);
          eidx[i].ei_leaf_lo = htole32(
              (uint32_t)(current_level[start + i].block_num & 0xFFFFFFFF));
          eidx[i].ei_leaf_hi =
              htole16((uint16_t)(current_level[start + i].block_num >> 32));
          eidx[i].ei_unused = 0;
        }

        if (device_write(dev, blk * block_size, idx_buf, block_size) < 0) {
          free(idx_buf);
          free(next_level);
          free(current_level);
          free(exts);
          return -1;
        }
        free(idx_buf);
      }

      free(current_level);
      current_level = next_level;
      current_count = num_idx;
      depth++;
    }

    if (depth > 1) {
      printf("  inode %lu: %d extents → depth-%u extent tree "
             "(%u index levels)\n",
             (unsigned long)fe->ino, ext_count, depth, depth - 1);
    }

    /* --- Step 3: write inode root index --- */
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    eh->eh_magic = htole16(EXT4_EXT_MAGIC);
    eh->eh_entries = htole16((uint16_t)current_count);
    eh->eh_max = htole16(INLINE_EXTENT_MAX);
    eh->eh_depth = htole16(depth);
    eh->eh_generation = htole32(0);

    struct ext4_extent_idx *idx =
        (struct ext4_extent_idx *)((uint8_t *)inode->i_block +
                                   sizeof(struct ext4_extent_header));
    for (uint32_t i = 0; i < current_count; i++) {
      idx[i].ei_block = htole32(current_level[i].first_file_block);
      idx[i].ei_leaf_lo =
          htole32((uint32_t)(current_level[i].block_num & 0xFFFFFFFF));
      idx[i].ei_leaf_hi = htole16((uint16_t)(current_level[i].block_num >> 32));
      idx[i].ei_unused = 0;
    }

    free(current_level);
  }

  inode->i_flags |= htole32(EXT4_EXTENTS_FL);
  free(exts);
  return 0;
}
