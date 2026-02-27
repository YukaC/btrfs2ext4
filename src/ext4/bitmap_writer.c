/*
 * bitmap_writer.c — Ext4 block and inode bitmap writer
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_io.h"
#include "ext4/ext4_crc16.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

/* Set a bit in a bitmap buffer (bounded to block_size width) */
static inline void bitmap_set(uint8_t *bitmap, uint64_t bit,
                              uint32_t max_bits) {
  if (bit < max_bits)
    bitmap[bit / 8] |= (1 << (bit % 8));
}

int ext4_write_bitmaps(struct device *dev, const struct ext4_layout *layout,
                       const struct ext4_block_allocator *alloc,
                       const struct inode_map *inode_map) {
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
            bitmap_set(block_bitmap, local, 8 * block_size);
        }
      }
    }

    /* Bug P fix: Mark bits beyond total_blocks in the last group as "used".
     * The last group may be partial — bits for blocks that don't exist on
     * disk must be set to 1, otherwise e2fsck will count them as free. */
    if (g == layout->num_groups - 1) {
      uint64_t bits_in_group = layout->total_blocks - group_start;
      for (uint64_t b = bits_in_group; b < layout->blocks_per_group; b++) {
        if (b < (uint64_t)(8 * block_size))
          bitmap_set(block_bitmap, b, 8 * block_size);
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
        bitmap_set(inode_bitmap, i, 8 * block_size);
      }
    }

    /* Bug A fix: Mark all active inodes as used in the bitmap.
     * Previously the inode bitmap was written with only reserved inodes,
     * leaving all real inodes (11..N) as "free". This caused e2fsck to
     * delete all user files on the first check. */
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

  /* Read Superblock early to get UUID for checksums */
  struct ext4_super_block sb;
  if (device_read(dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) < 0) {
    return -1;
  }

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

    /* Bug C fix: Update GDT using layout->desc_size as stride.
     * Previously used sizeof(struct ext4_group_desc) which is 32 bytes,
     * but in 64-bit mode each descriptor is 64 bytes. Using the wrong
     * stride corrupted every other group descriptor's high fields. */
    uint64_t gdt_offset = layout->groups[0].gdt_start_block * block_size +
                          (uint64_t)g * layout->desc_size;

    uint8_t gd_buf[64];
    memset(gd_buf, 0, sizeof(gd_buf));

    if (device_read(dev, gdt_offset, gd_buf, layout->desc_size) < 0) {
      free(bitmap);
      return -1;
    }

    /* Modify free counts at known offsets (bg_free_blocks_count_lo @ 12,
     * bg_free_inodes_count_lo @ 14) */
    *(uint16_t *)(gd_buf + 12) = htole16((uint16_t)(free_blocks & 0xFFFF));
    *(uint16_t *)(gd_buf + 14) = htole16((uint16_t)(free_inodes & 0xFFFF));

    /* Calculate GDT checksum if CSUM feature is enabled
     * (We always enable EXT4_FEATURE_RO_COMPAT_GDT_CSUM)
     * bg_checksum = crc16(uuid + group_number + gdt_desc) */
    struct ext4_group_desc *desc = (struct ext4_group_desc *)gd_buf;
    desc->bg_checksum = 0; /* Seed with 0 for calculation */

    uint16_t crc = ext4_crc16(~0, sb.s_uuid, sizeof(sb.s_uuid));
    uint32_t le_group = htole32(g);
    crc = ext4_crc16(crc, &le_group, sizeof(le_group));
    crc = ext4_crc16(crc, desc, layout->desc_size);
    desc->bg_checksum = htole16(crc);

    if (device_write(dev, gdt_offset, gd_buf, layout->desc_size) < 0) {
      free(bitmap);
      return -1;
    }
  }

  /* Final Superblock Update */
  sb.s_free_blocks_count_lo =
      htole32((uint32_t)(total_free_blocks & 0xFFFFFFFF));
  sb.s_free_inodes_count = htole32((uint32_t)(total_free_inodes & 0xFFFFFFFF));

  if (device_write(dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) < 0) {
    free(bitmap);
    return -1;
  }

  free(bitmap);
  printf("  Total free blocks: %lu\n", (unsigned long)total_free_blocks);
  printf("  Total free inodes: %lu\n", (unsigned long)total_free_inodes);

  return 0;
}
