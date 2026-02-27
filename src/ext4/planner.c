/*
 * planner.c — Ext4 layout planner
 *
 * Calculates the ext4 block group layout for a given device size.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/chunk_tree.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"

int ext4_plan_layout(struct ext4_layout *layout, uint64_t device_size,
                     uint32_t block_size, uint32_t inode_ratio,
                     const struct btrfs_fs_info *fs_info) {
  memset(layout, 0, sizeof(*layout));

  if (block_size == 0)
    block_size = EXT4_DEFAULT_BLOCK_SIZE;
  if (inode_ratio == 0)
    inode_ratio = EXT4_DEFAULT_INODE_RATIO;

  /* Reject zero-size or impossibly small devices */
  if (device_size == 0 || device_size < block_size) {
    fprintf(stderr, "btrfs2ext4: device too small (%lu bytes)\n",
            (unsigned long)device_size);
    return -1;
  }

  layout->block_size = block_size;

  uint64_t total_blocks;
  if (__builtin_mul_overflow(device_size, 1, &total_blocks) ||
      device_size / block_size == 0) {
    fprintf(stderr, "btrfs2ext4: block calculation overflow or zero blocks\n");
    return -1;
  }
  layout->total_blocks = device_size / block_size;
  layout->blocks_per_group = 8 * block_size; /* bits in one block */
  layout->inode_size = EXT4_DEFAULT_INODE_SIZE;
  layout->desc_size = 64; /* 64-bit ext4 */

  /* Number of block groups */
  layout->num_groups = (layout->total_blocks + layout->blocks_per_group - 1) /
                       layout->blocks_per_group;

  /* Inodes per group */
  uint64_t total_inodes_raw = device_size / inode_ratio;

  /* Security override: Enforce enough inodes for all files + reserved nodes */
  if (fs_info && total_inodes_raw < fs_info->inode_count + 16) {
    total_inodes_raw = fs_info->inode_count + 16;
  }

  /* Round up to multiple of num_groups */
  layout->inodes_per_group =
      (total_inodes_raw + layout->num_groups - 1) / layout->num_groups;
  /* Must be multiple of 8 (for bitmap) */
  layout->inodes_per_group = (layout->inodes_per_group + 7) & ~7U;
  /* Max inodes per group = 8 * block_size (one bitmap block) */
  if (layout->inodes_per_group > 8 * block_size)
    layout->inodes_per_group = 8 * block_size;
  /* Minimum reasonable value */
  if (layout->inodes_per_group < 16)
    layout->inodes_per_group = 16;

  layout->total_inodes = layout->inodes_per_group * layout->num_groups;

  /*
   * Security constraint: If the Btrfs filesystem has more tiny files than the
   * physical Ext4 group architecture can comfortably map, we must abort
   * instantly. Silently proceeding would write Ext4 inode structures well
   * out-of-bounds.
   */
  if (fs_info && layout->total_inodes < fs_info->inode_count + 16) {
    fprintf(stderr,
            "\n[FATAL] btrfs2ext4: Architecture Limitation Exceeded!\n");
    fprintf(stderr,
            "This filesystem has %u inodes, but the physical Ext4 "
            "geometry at this device size can only support %u inodes.\n",
            fs_info->inode_count, layout->total_inodes);
    return -1;
  }

  /*
   * Pre-calculate actual utilized space & Data blocks scaling footprint:
   * Ext4 requires physically allocating blocks for index trees and
   * long symlinks, while ignoring sparse holes.
   */
  uint64_t data_blocks_required = 0;
  if (fs_info) {
    for (uint32_t i = 0; i < fs_info->inode_count; i++) {
      struct file_entry *fe = fs_info->inode_table[i];

      if (fe->mode & S_IFLNK) {
        if (fe->size > 59) {
          data_blocks_required++; /* Symlinks > 59B take 1 data block */
        }
      } else if (fe->mode & S_IFREG) {
        /* Extent tree index blocks for fragmented files */
        if (fe->extent_count > 4) {
          /* Each extent node takes roughly 340 extents (4096 / 12) */
          uint32_t index_blocks = (fe->extent_count + 339) / 340;
          data_blocks_required += index_blocks;
        }

        /* Actual data blocks (ignoring sparse holes) */
        for (uint32_t e = 0; e < fe->extent_count; e++) {
          struct file_extent *ext = &fe->extents[e];
          if (ext->type != BTRFS_FILE_EXTENT_INLINE && ext->disk_bytenr != 0) {
            data_blocks_required +=
                (ext->num_bytes + block_size - 1) / block_size;
          }
        }
      } else if (fe->mode & S_IFDIR) {
        /* Base directory size */
        data_blocks_required += (fe->size + block_size - 1) / block_size;
      }
    }
  }

  printf("=== Ext4 Constraints & Pre-Calculation ===\n");
  printf("  Device size:       %lu bytes (%.1f GiB)\n",
         (unsigned long)device_size,
         (double)device_size / (1024.0 * 1024.0 * 1024.0));
  printf("  Block size:        %u\n", layout->block_size);
  printf("  Total blocks:      %lu\n", (unsigned long)layout->total_blocks);
  printf("  Blocks per group:  %u\n", layout->blocks_per_group);
  printf("  Number of groups:  %u\n", layout->num_groups);
  printf("  Inodes per group:  %u\n", layout->inodes_per_group);
  printf("  Total inodes:      %u\n", layout->total_inodes);
  printf("  Inode size:        %u\n", layout->inode_size);

  /* Allocate group layouts */
  layout->groups = calloc(layout->num_groups, sizeof(struct ext4_bg_layout));
  if (!layout->groups) {
    fprintf(stderr, "btrfs2ext4: out of memory for group layouts\n");
    return -1;
  }

  /* How many blocks does the GDT occupy? */
  uint32_t gdt_blocks =
      (layout->num_groups * layout->desc_size + block_size - 1) / block_size;

  /* Reserved GDT blocks for future growth */
  uint32_t reserved_gdt = 0;
  if (layout->total_blocks > 1024)
    reserved_gdt = gdt_blocks; /* Reserve same amount for growth */

  /* Inode table blocks per group */
  uint32_t inode_table_blocks =
      (layout->inodes_per_group * layout->inode_size + block_size - 1) /
      block_size;

  /* Initialize reserved blocks list */
  layout->reserved_block_capacity = 1024;
  layout->reserved_blocks =
      calloc(layout->reserved_block_capacity, sizeof(uint64_t));
  if (!layout->reserved_blocks) {
    free(layout->groups);
    return -1;
  }

  /* Plan each block group */
  for (uint32_t g = 0; g < layout->num_groups; g++) {
    struct ext4_bg_layout *bg = &layout->groups[g];
    bg->group_start_block = (uint64_t)g * layout->blocks_per_group;

    /* For block_size=4096, first_data_block = 0 */
    uint64_t first_block = bg->group_start_block;
    if (g == 0 && block_size > 1024)
      first_block = 0; /* superblock is at byte 1024, which is within block 0 */

    uint64_t cursor = first_block;

    /* Does this group have a superblock backup? */
    bg->has_super = ext4_bg_has_super(g);

    if (bg->has_super) {
      bg->superblock_block = cursor;
      cursor++; /* superblock = 1 block */

      bg->gdt_start_block = cursor;
      bg->gdt_blocks = gdt_blocks;
      cursor += gdt_blocks;

      bg->reserved_gdt_blocks = reserved_gdt;
      cursor += reserved_gdt;

      /* Mark these as reserved */
      for (uint64_t b = first_block; b < cursor; b++) {
        if (layout->reserved_block_count >= layout->reserved_block_capacity) {
          uint32_t new_cap = layout->reserved_block_capacity * 2;
          uint64_t *new_blocks =
              realloc(layout->reserved_blocks, new_cap * sizeof(uint64_t));
          if (!new_blocks) {
            fprintf(stderr,
                    "btrfs2ext4: OOM reallocating reserved blocks (gdt)\n");
            free(layout->groups);
            free(layout->reserved_blocks);
            return -1;
          }
          layout->reserved_blocks = new_blocks;
          layout->reserved_block_capacity = new_cap;
        }
        layout->reserved_blocks[layout->reserved_block_count++] = b;
      }
    }

    /* Block bitmap (1 block) */
    bg->block_bitmap_block = cursor;
    cursor++;

    /* Inode bitmap (1 block) */
    bg->inode_bitmap_block = cursor;
    cursor++;

    /* Inode table */
    bg->inode_table_start = cursor;
    bg->inode_table_blocks = inode_table_blocks;
    cursor += inode_table_blocks;

    /* Mark bitmaps and inode table as reserved */
    for (uint64_t b = bg->block_bitmap_block; b < cursor; b++) {
      if (layout->reserved_block_count >= layout->reserved_block_capacity) {
        uint32_t new_cap = layout->reserved_block_capacity * 2;
        uint64_t *new_blocks =
            realloc(layout->reserved_blocks, new_cap * sizeof(uint64_t));
        if (!new_blocks) {
          fprintf(stderr, "btrfs2ext4: OOM reallocating reserved blocks "
                          "(bitmaps/itable)\n");
          free(layout->groups);
          free(layout->reserved_blocks);
          return -1;
        }
        layout->reserved_blocks = new_blocks;
        layout->reserved_block_capacity = new_cap;
      }
      layout->reserved_blocks[layout->reserved_block_count++] = b;
    }

    /* Remaining blocks are data */
    uint64_t group_end = bg->group_start_block + layout->blocks_per_group;
    if (group_end > layout->total_blocks)
      group_end = layout->total_blocks;

    bg->data_start_block = cursor;
    bg->data_blocks = (cursor < group_end) ? (uint32_t)(group_end - cursor) : 0;
  }

  printf("  Reserved blocks:   %u (metadata zones)\n",
         layout->reserved_block_count);
  printf("  Data blocks req:   %lu (files, index, dirs)\n",
         (unsigned long)data_blocks_required);

  /*
   * Phase 2.2: Deadlock Prevention (The 5% Rule)
   * Verify we have enough actual physical Free Space to proceed safely.
   */
  uint64_t physically_usable =
      layout->total_blocks - layout->reserved_block_count;
  if (data_blocks_required >= physically_usable) {
    fprintf(stderr,
            "\n[FATAL] btrfs2ext4: Insufficient space for conversion!\n");
    fprintf(stderr, "  Total blocks: %lu\n",
            (unsigned long)layout->total_blocks);
    fprintf(stderr, "  Metadata rsrv:%u\n", layout->reserved_block_count);
    fprintf(stderr, "  Data to write:%lu\n",
            (unsigned long)data_blocks_required);
    free(layout->groups);
    free(layout->reserved_blocks);
    return -1;
  }

  uint64_t free_blocks = physically_usable - data_blocks_required;
  uint64_t margin = layout->total_blocks / 20; /* 5% */

  if (free_blocks < margin && margin > 0) {
    fprintf(stderr, "\n[FATAL] btrfs2ext4: Conversion blocked by Deadlock "
                    "Prevention Rule!\n");
    fprintf(stderr,
            "Calculated free space (%lu MiB) falls below the safety margin of "
            "5%% (%lu MiB).\n",
            (unsigned long)(free_blocks * block_size) / (1024 * 1024),
            (unsigned long)(margin * block_size) / (1024 * 1024));
    free(layout->groups);
    free(layout->reserved_blocks);
    return -1;
  }

  printf("  Free Space Margin: %lu blocks (%.1f MiB)\n",
         (unsigned long)free_blocks,
         (double)(free_blocks * block_size) / (1024.0 * 1024.0));
  printf("========================\n\n");

  return 0;
}

uint32_t ext4_find_conflicts(const struct ext4_layout *layout,
                             const struct btrfs_fs_info *fs_info) {
  /*
   * Find btrfs data extents that overlap with ext4 reserved (metadata) blocks.
   * This is the set of blocks that need to be relocated.
   */
  uint32_t conflicts = 0;
  uint32_t block_size = layout->block_size;

  /* Pre-build a bitmap of reserved blocks for O(1) lookups */
  uint8_t *bitmap = calloc((layout->total_blocks + 7) / 8, 1);
  if (!bitmap) {
    fprintf(stderr, "btrfs2ext4: out of memory allocating conflict bitmap\n");
    return 0;
  }

  for (uint32_t i = 0; i < layout->reserved_block_count; i++) {
    uint64_t b = layout->reserved_blocks[i];
    if (b < layout->total_blocks) {
      bitmap[b / 8] |= (1 << (b % 8));
    }
  }

  /* For each file entry in the btrfs filesystem */
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    const struct file_entry *fe = fs_info->inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      const struct file_extent *ext = &fe->extents[j];
      if (ext->type == BTRFS_FILE_EXTENT_INLINE || ext->disk_bytenr == 0)
        continue;

      /* Convert the extent's logical address to physical */
      uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
      if (phys == (uint64_t)-1)
        continue;

      uint64_t start_block = phys / block_size;
      uint64_t end_block =
          (phys + ext->disk_num_bytes + block_size - 1) / block_size;

      /* O(1) bitmap check for conflicts across blocks in extent */
      for (uint64_t b = start_block; b < end_block; b++) {
        if (b < layout->total_blocks && (bitmap[b / 8] & (1 << (b % 8)))) {
          conflicts++;
          break; /* Count each extent only once */
        }
      }
    }
  }

  free(bitmap);
  printf("Found %u data extents conflicting with ext4 metadata zones\n\n",
         conflicts);
  return conflicts;
}

void ext4_free_layout(struct ext4_layout *layout) {
  free(layout->groups);
  free(layout->reserved_blocks);
  memset(layout, 0, sizeof(*layout));
}
