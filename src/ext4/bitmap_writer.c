/*
 * bitmap_writer.c — Ext4 block and inode bitmap writer
 *
 * Approach B (Phase 1): ext4_finalize_bitmaps() writes block+inode bitmaps
 * from in-memory allocator state and inode_map as the LAST metadata write
 * before ext4_update_free_counts().
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_io.h"
#include "ext4/ext4_crc16.h"
#include "ext4/ext4_metadata_csum.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"
#include "term_ui.h"

static inline void bitmap_set(uint8_t *bitmap, uint64_t bit,
                              uint32_t max_bits) {
  if (bit < max_bits)
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static int write_block_bitmap_group(struct device *dev,
                                    const struct ext4_layout *layout,
                                    const struct ext4_block_allocator *alloc,
                                    uint32_t g) {
  uint32_t block_size = layout->block_size;
  const struct ext4_bg_layout *bg = &layout->groups[g];

  uint8_t *block_bitmap = calloc(1, block_size);
  if (!block_bitmap)
    return -1;

  uint64_t group_start = bg->group_start_block;
  uint64_t group_end = group_start + layout->blocks_per_group;
  if (group_end > layout->total_blocks)
    group_end = layout->total_blocks;

  if (alloc && alloc->reserved_bitmap) {
    for (uint64_t b = group_start; b < group_end; b++) {
      if (alloc->reserved_bitmap[b / 8] & (1 << (b % 8))) {
        uint64_t local = b - group_start;
        if (local < (uint64_t)(8 * block_size))
          bitmap_set(block_bitmap, local, 8 * block_size);
      }
    }
  }

  if (g == layout->num_groups - 1) {
    uint64_t bits_in_group = layout->total_blocks - group_start;
    for (uint64_t b = bits_in_group; b < layout->blocks_per_group; b++) {
      if (b < (uint64_t)(8 * block_size))
        bitmap_set(block_bitmap, b, 8 * block_size);
    }
  }

  if (device_write(dev, bg->block_bitmap_block * block_size, block_bitmap,
                   block_size) < 0) {
    free(block_bitmap);
    return -1;
  }
  free(block_bitmap);
  return 0;
}

static int write_inode_bitmap_group(struct device *dev,
                                    const struct ext4_layout *layout,
                                    const struct inode_map *inode_map,
                                    uint32_t g) {
  uint32_t block_size = layout->block_size;
  const struct ext4_bg_layout *bg = &layout->groups[g];

  uint8_t *inode_bitmap = calloc(1, block_size);
  if (!inode_bitmap)
    return -1;

  if (g == 0) {
    for (uint32_t i = 0; i < EXT4_GOOD_OLD_FIRST_INO - 1; i++) {
      bitmap_set(inode_bitmap, i, 8 * block_size);
    }
  }

  if (inode_map) {
    uint32_t ino_start = g * layout->inodes_per_group + 1;
    uint32_t ino_end = ino_start + layout->inodes_per_group;
    for (uint32_t idx = 0; idx < inode_map->count; idx++) {
      uint32_t ext4_ino = inode_map->entries[idx].ext4_ino;
      if (ext4_ino >= ino_start && ext4_ino < ino_end) {
        uint32_t local_bit = ext4_ino - ino_start;
        bitmap_set(inode_bitmap, local_bit, 8 * block_size);
      }
    }
  }

  if (device_write(dev, bg->inode_bitmap_block * block_size, inode_bitmap,
                   block_size) < 0) {
    free(inode_bitmap);
    return -1;
  }
  free(inode_bitmap);
  return 0;
}

static int write_bitmaps_from_state(struct device *dev,
                                    const struct ext4_layout *layout,
                                    const struct ext4_block_allocator *alloc,
                                    const struct inode_map *inode_map) {
  for (uint32_t g = 0; g < layout->num_groups; g++) {
    if (write_block_bitmap_group(dev, layout, alloc, g) < 0)
      return -1;
    if (write_inode_bitmap_group(dev, layout, inode_map, g) < 0)
      return -1;
  }
  return 0;
}

int ext4_write_bitmaps(struct device *dev, const struct ext4_layout *layout,
                       const struct ext4_block_allocator *alloc,
                       const struct inode_map *inode_map) {
  term_log_debug("Writing block and inode bitmaps...\n");
  if (write_bitmaps_from_state(dev, layout, alloc, inode_map) < 0)
    return -1;
  term_log_debug("  Bitmaps written for %u groups\n", layout->num_groups);
  return 0;
}

int ext4_finalize_bitmaps(struct device *dev, struct ext4_layout *layout,
                          const struct ext4_block_allocator *alloc,
                          const struct inode_map *inode_map) {
  term_log_debug("Finalizing block and inode bitmaps from in-memory state...\n");
  if (write_bitmaps_from_state(dev, layout, alloc, inode_map) < 0)
    return -1;
  ext4_update_gdt_used_dirs(dev, layout);
  term_log_debug("  Bitmaps finalized for %u groups\n", layout->num_groups);
  return 0;
}

int ext4_update_free_counts(struct device *dev,
                            const struct ext4_layout *layout) {
  uint32_t block_size = layout->block_size;
  uint64_t total_free_blocks = 0;
  uint64_t total_free_inodes = 0;

  term_log_debug("Calculating true free blocks and inodes...\n");

  struct ext4_super_block sb;
  if (device_read(dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) < 0) {
    return -1;
  }

  uint32_t ro_compat = le32toh(sb.s_feature_ro_compat);
  int metadata_csum = !!(ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);
  uint32_t csum_seed = le32toh(sb.s_checksum_seed);

  uint8_t *bitmap = malloc(block_size);
  if (!bitmap)
    return -1;

  for (uint32_t g = 0; g < layout->num_groups; g++) {
    const struct ext4_bg_layout *bg = &layout->groups[g];

    if (device_read(dev, bg->block_bitmap_block * block_size, bitmap,
                    block_size) < 0) {
      free(bitmap);
      return -1;
    }

    uint32_t free_blocks = 0;
    uint32_t bits_to_check =
        (g == layout->num_groups - 1)
            ? (layout->total_blocks - bg->group_start_block)
            : layout->blocks_per_group;

    for (uint32_t i = 0; i < bits_to_check; i++) {
      if (!(bitmap[i / 8] & (1 << (i % 8)))) {
        free_blocks++;
      }
    }
    total_free_blocks += free_blocks;

    if (device_read(dev, bg->inode_bitmap_block * block_size, bitmap,
                    block_size) < 0) {
      free(bitmap);
      return -1;
    }

    uint32_t free_inodes = 0;
    uint32_t inodes_to_check =
        (g == layout->num_groups - 1)
            ? (layout->total_inodes - g * layout->inodes_per_group)
            : layout->inodes_per_group;

    for (uint32_t i = 0; i < inodes_to_check; i++) {
      if (!(bitmap[i / 8] & (1 << (i % 8)))) {
        free_inodes++;
      }
    }
    total_free_inodes += free_inodes;

    uint64_t gdt_offset = layout->groups[0].gdt_start_block * block_size +
                          (uint64_t)g * layout->desc_size;

    uint8_t gd_buf[64];
    memset(gd_buf, 0, sizeof(gd_buf));

    if (device_read(dev, gdt_offset, gd_buf, layout->desc_size) < 0) {
      free(bitmap);
      return -1;
    }

    *(uint16_t *)(gd_buf + 12) = htole16((uint16_t)(free_blocks & 0xFFFF));
    *(uint16_t *)(gd_buf + 14) = htole16((uint16_t)(free_inodes & 0xFFFF));

    struct ext4_group_desc *desc = (struct ext4_group_desc *)gd_buf;
    desc->bg_checksum = 0;

    if (metadata_csum) {
      desc->bg_checksum =
          htole16(ext4_group_desc_csum(csum_seed, g, desc, layout->desc_size));
    } else {
      uint16_t crc = ext4_crc16((uint16_t)~0, sb.s_uuid, sizeof(sb.s_uuid));
      uint32_t le_group = htole32(g);
      crc = ext4_crc16(crc, &le_group, sizeof(le_group));
      crc = ext4_crc16(crc, desc, layout->desc_size);
      desc->bg_checksum = htole16(crc);
    }

    if (device_write(dev, gdt_offset, gd_buf, layout->desc_size) < 0) {
      free(bitmap);
      return -1;
    }
  }

  sb.s_free_blocks_count_lo =
      htole32((uint32_t)(total_free_blocks & 0xFFFFFFFF));
  sb.s_free_inodes_count = htole32((uint32_t)(total_free_inodes & 0xFFFFFFFF));

  if (metadata_csum)
    ext4_superblock_csum_set(&sb);

  if (device_write(dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) < 0) {
    free(bitmap);
    return -1;
  }

  free(bitmap);
  term_log_debug("  Total free blocks: %lu\n", (unsigned long)total_free_blocks);
  term_log_debug("  Total free inodes: %lu\n", (unsigned long)total_free_inodes);

  return 0;
}
