/*
 * ext4_planner.h — Ext4 layout planner API
 */

#ifndef EXT4_PLANNER_H
#define EXT4_PLANNER_H

#include <stdint.h>

/* Represents one block group's metadata layout */
struct ext4_bg_layout {
  uint64_t group_start_block;   /* first block of this group */
  uint64_t superblock_block;    /* superblock copy location (0 if none) */
  uint64_t gdt_start_block;     /* GDT start (0 if none) */
  uint32_t gdt_blocks;          /* number of GDT blocks */
  uint32_t reserved_gdt_blocks; /* reserved GDT growth blocks */
  uint64_t block_bitmap_block;  /* block bitmap location */
  uint64_t inode_bitmap_block;  /* inode bitmap location */
  uint64_t inode_table_start;   /* first block of inode table */
  uint32_t inode_table_blocks;  /* blocks occupied by inode table */
  uint64_t data_start_block;    /* first usable data block */
  uint32_t data_blocks;         /* number of data blocks */
  int has_super;                /* does this group have a superblock? */
};

/* Complete ext4 layout plan */
struct ext4_layout {
  uint64_t total_blocks;
  uint32_t block_size;       /* bytes per block (4096) */
  uint32_t blocks_per_group; /* blocks per group (32768) */
  uint32_t inodes_per_group;
  uint32_t inode_size; /* bytes per inode (256) */
  uint32_t num_groups; /* number of block groups */
  uint32_t total_inodes;
  uint16_t desc_size; /* group descriptor size (64) */

  struct ext4_bg_layout *groups; /* array of all group layouts */

  /* Reserved (metadata) blocks — these must be free of user data */
  uint64_t *reserved_blocks; /* list of block numbers */
  uint32_t reserved_block_count;
  uint32_t reserved_block_capacity;
};

struct btrfs_fs_info;

/*
 * Calculate the ext4 layout for a device.
 * device_size is in bytes, inode_ratio is bytes per inode.
 * Returns 0 on success, -1 on error.
 */
int ext4_plan_layout(struct ext4_layout *layout, uint64_t device_size,
                     uint32_t block_size, uint32_t inode_ratio,
                     const struct btrfs_fs_info *fs_info);

/*
 * Find all reserved (metadata) block numbers that conflict with
 * btrfs data extents.
 * Returns the number of conflicts found.
 */
uint32_t ext4_find_conflicts(const struct ext4_layout *layout,
                             const struct btrfs_fs_info *fs_info);

/*
 * Free layout resources.
 */
void ext4_free_layout(struct ext4_layout *layout);

#endif /* EXT4_PLANNER_H */
