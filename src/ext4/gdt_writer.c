/*
 * gdt_writer.c — Ext4 Group Descriptor Table writer
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"

int ext4_write_gdt(struct device *dev, const struct ext4_layout *layout) {
  uint32_t block_size = layout->block_size;
  uint32_t gdt_size = layout->num_groups * layout->desc_size;
  uint32_t gdt_blocks = (gdt_size + block_size - 1) / block_size;

  uint8_t *gdt_buf = calloc(gdt_blocks, block_size);
  if (!gdt_buf) {
    fprintf(stderr, "btrfs2ext4: out of memory for GDT buffer\n");
    return -1;
  }

  /* Fill in group descriptors */
  for (uint32_t g = 0; g < layout->num_groups; g++) {
    struct ext4_group_desc *desc =
        (struct ext4_group_desc *)(gdt_buf + g * layout->desc_size);
    const struct ext4_bg_layout *bg = &layout->groups[g];

    desc->bg_block_bitmap_lo =
        htole32((uint32_t)(bg->block_bitmap_block & 0xFFFFFFFF));
    desc->bg_block_bitmap_hi =
        htole32((uint32_t)(bg->block_bitmap_block >> 32));
    desc->bg_inode_bitmap_lo =
        htole32((uint32_t)(bg->inode_bitmap_block & 0xFFFFFFFF));
    desc->bg_inode_bitmap_hi =
        htole32((uint32_t)(bg->inode_bitmap_block >> 32));
    desc->bg_inode_table_lo =
        htole32((uint32_t)(bg->inode_table_start & 0xFFFFFFFF));
    desc->bg_inode_table_hi = htole32((uint32_t)(bg->inode_table_start >> 32));

    /* Free counts will be set during bitmap generation.
     * For now, set to full capacity as placeholders. */
    desc->bg_free_blocks_count_lo =
        htole16((uint16_t)(bg->data_blocks & 0xFFFF));
    desc->bg_free_blocks_count_hi = htole16((uint16_t)(bg->data_blocks >> 16));
    desc->bg_free_inodes_count_lo =
        htole16((uint16_t)(layout->inodes_per_group & 0xFFFF));
    desc->bg_free_inodes_count_hi = htole16(0);
    desc->bg_used_dirs_count_lo = htole16(0);

    /* Flags: uninit for optimization */
    desc->bg_flags = htole16(0);
  }

  printf("Writing GDT (%u groups, %u blocks)...\n", layout->num_groups,
         gdt_blocks);

  /* Write GDT to each block group that has a superblock */
  for (uint32_t g = 0; g < layout->num_groups; g++) {
    if (!layout->groups[g].has_super)
      continue;

    uint64_t gdt_offset = layout->groups[g].gdt_start_block * block_size;
    if (device_write(dev, gdt_offset, gdt_buf, gdt_blocks * block_size) < 0) {
      free(gdt_buf);
      return -1;
    }
  }

  free(gdt_buf);
  printf("  GDT written to all superblock groups\n");
  return 0;
}
