/*
 * bitmap_writer.c — Ext4 block and inode bitmap writer
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

/* Set a bit in a bitmap buffer */
static inline void bitmap_set(uint8_t *bitmap, uint64_t bit) {
  bitmap[bit / 8] |= (1 << (bit % 8));
}

int ext4_write_bitmaps(struct device *dev, const struct ext4_layout *layout,
                       const struct ext4_block_allocator *alloc) {
  uint32_t block_size = layout->block_size;

  printf("Writing block and inode bitmaps...\n");

  for (uint32_t g = 0; g < layout->num_groups; g++) {
    const struct ext4_bg_layout *bg = &layout->groups[g];

    /* --- Block bitmap --- */
    uint8_t *block_bitmap = calloc(1, block_size);
    if (!block_bitmap)
      return -1;

    uint64_t group_start = bg->group_start_block;
    uint64_t group_end = group_start + layout->blocks_per_group;
    if (group_end > layout->total_blocks)
      group_end = layout->total_blocks;

    /* Marcar todos los bloques utilizados según el bitmap global del
     * allocator (incluye metadatos y datos, tanto Btrfs reutilizados
     * como bloques nuevos asignados por Ext4). */
    if (alloc && alloc->reserved_bitmap) {
      for (uint64_t b = group_start; b < group_end; b++) {
        if (alloc->reserved_bitmap[b / 8] & (1 << (b % 8))) {
          uint64_t local = b - group_start;
          if (local < (uint64_t)(8 * block_size))
            bitmap_set(block_bitmap, local);
        }
      }
    }

    /* Mark blocks beyond the last group as used (partial last group) */
    uint64_t last_group_end = group_start + layout->blocks_per_group;
    if (last_group_end > layout->total_blocks) {
      for (uint64_t b = layout->total_blocks; b < group_end; b++) {
        uint64_t local = b - group_start;
        if (local < (uint64_t)(8 * block_size))
          bitmap_set(block_bitmap, local);
      }
    }

    /* Write block bitmap */
    if (device_write(dev, bg->block_bitmap_block * block_size, block_bitmap,
                     block_size) < 0) {
      free(block_bitmap);
      return -1;
    }
    free(block_bitmap);

    /* --- Inode bitmap --- */
    uint8_t *inode_bitmap = calloc(1, block_size);
    if (!inode_bitmap)
      return -1;

    /* Mark reserved inodes as used (inodes 1-10 in group 0) */
    if (g == 0) {
      for (uint32_t i = 0; i < EXT4_GOOD_OLD_FIRST_INO - 1; i++) {
        bitmap_set(inode_bitmap, i);
      }
    }

    /* Write inode bitmap */
    if (device_write(dev, bg->inode_bitmap_block * block_size, inode_bitmap,
                     block_size) < 0) {
      free(inode_bitmap);
      return -1;
    }
    free(inode_bitmap);
  }

  printf("  Bitmaps written for %u groups\n", layout->num_groups);
  return 0;
}

int ext4_update_free_counts(struct device *dev,
                            const struct ext4_layout *layout) {
  uint32_t block_size = layout->block_size;
  uint64_t total_free_blocks = 0;
  uint64_t total_free_inodes = 0;

  printf("Calculating true free blocks and inodes...\n");

  uint8_t *bitmap = malloc(block_size);
  if (!bitmap)
    return -1;

  for (uint32_t g = 0; g < layout->num_groups; g++) {
    const struct ext4_bg_layout *bg = &layout->groups[g];

    /* Read block bitmap */
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

    /* Read inode bitmap */
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

    /* Update GDT (Read-Modify-Write) */
    struct ext4_group_desc gd;
    uint64_t gdt_offset = layout->groups[0].group_start_block * block_size +
                          (layout->groups[0].has_super ? block_size : 0) +
                          g * sizeof(struct ext4_group_desc);

    if (device_read(dev, gdt_offset, &gd, sizeof(gd)) < 0) {
      free(bitmap);
      return -1;
    }

    gd.bg_free_blocks_count_lo = htole16((uint16_t)(free_blocks & 0xFFFF));
    gd.bg_free_inodes_count_lo = htole16((uint16_t)(free_inodes & 0xFFFF));

    if (device_write(dev, gdt_offset, &gd, sizeof(gd)) < 0) {
      free(bitmap);
      return -1;
    }
  }

  /* Update Superblock */
  struct ext4_super_block sb;
  if (device_read(dev, 1024, &sb, sizeof(sb)) < 0) {
    free(bitmap);
    return -1;
  }

  sb.s_free_blocks_count_lo =
      htole32((uint32_t)(total_free_blocks & 0xFFFFFFFF));
  sb.s_free_inodes_count = htole32((uint32_t)(total_free_inodes & 0xFFFFFFFF));

  if (device_write(dev, 1024, &sb, sizeof(sb)) < 0) {
    free(bitmap);
    return -1;
  }

  free(bitmap);
  printf("  Total free blocks: %lu\n", (unsigned long)total_free_blocks);
  printf("  Total free inodes: %lu\n", (unsigned long)total_free_inodes);

  return 0;
}
