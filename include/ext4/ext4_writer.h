/*
 * ext4_writer.h — Ext4 metadata writer API
 */

#ifndef EXT4_WRITER_H
#define EXT4_WRITER_H

#include <stddef.h>
#include <stdint.h>

struct device;
struct ext4_layout;
struct btrfs_fs_info;

/* Allocator state structure (thread-safe, explicit state) */
struct ext4_block_allocator {
  uint64_t next_alloc_block;
  uint64_t max_blocks;
  /* 1 bit por bloque físico: 1 = bloque en uso (meta o datos) */
  uint8_t *reserved_bitmap;
};

/* Inode mapping: btrfs objectid → ext4 inode number */
struct inode_map_entry {
  uint64_t btrfs_ino;
  uint32_t ext4_ino;
};

struct inode_map {
  struct inode_map_entry *entries;
  uint32_t count;
  uint32_t capacity;
  /* Hash table for O(1) lookups */
  struct inode_map_entry *ht_buckets;
  uint32_t ht_size;

  /* mmap specific fields for extreme scalability */
  int fd_entries;
  int fd_ht;
  size_t mapped_entries_size;
  size_t mapped_ht_size;

  /* Adaptive memory and HDD thrashing prevention */
  struct adaptive_mem_config *mem_cfg;
  struct bloom_filter *bloom;
};

/*
 * Write the complete ext4 filesystem structures to the device.
 * This is the main "Pass 3" function that writes all ext4 metadata.
 *
 * Parameters:
 *   dev        - opened device handle
 *   layout     - pre-calculated ext4 layout
 *   fs_info    - btrfs filesystem data (source of truth for files/dirs)
 *   inode_map  - btrfs→ext4 inode number mapping (populated by this function)
 *
 * Returns 0 on success, -1 on error.
 */
int ext4_write_filesystem(struct device *dev, const struct ext4_layout *layout,
                          const struct btrfs_fs_info *fs_info,
                          struct inode_map *inode_map);

/* Individual writer functions (called internally by ext4_write_filesystem) */
int ext4_write_superblock(struct device *dev, const struct ext4_layout *layout,
                          const struct btrfs_fs_info *fs_info);
int ext4_write_gdt(struct device *dev, const struct ext4_layout *layout);
int ext4_write_bitmaps(struct device *dev, const struct ext4_layout *layout,
                       const struct ext4_block_allocator *alloc);
int ext4_update_free_counts(struct device *dev,
                            const struct ext4_layout *layout);
int ext4_write_inode_table(struct device *dev, const struct ext4_layout *layout,
                           const struct btrfs_fs_info *fs_info,
                           struct inode_map *inode_map,
                           struct ext4_block_allocator *alloc);
int ext4_write_directories(struct device *dev, const struct ext4_layout *layout,
                           const struct btrfs_fs_info *fs_info,
                           const struct inode_map *inode_map,
                           struct ext4_block_allocator *alloc);

void inode_map_free(struct inode_map *map);
int inode_map_add(struct inode_map *map, uint64_t btrfs_ino, uint32_t ext4_ino);
uint32_t inode_map_lookup(const struct inode_map *map, uint64_t btrfs_ino);

/* Block allocator for extent tree and directory blocks */
void ext4_block_alloc_init(struct ext4_block_allocator *alloc,
                           const struct ext4_layout *layout);
void ext4_block_alloc_free(struct ext4_block_allocator *alloc);
uint64_t ext4_alloc_block(struct ext4_block_allocator *alloc,
                          const struct ext4_layout *layout);

/* Marcar en el allocator todos los bloques de datos ya usados por Btrfs
 * (extents finales tras relocación) para que no sean reutilizados por Ext4. */
void ext4_block_alloc_mark_fs_data(struct ext4_block_allocator *alloc,
                                   const struct ext4_layout *layout,
                                   const struct btrfs_fs_info *fs_info);

/* Multi-level extent tree builder (replaces inline-only builder) */
struct ext4_inode;
struct file_entry;
struct chunk_map;
int ext4_build_extent_tree(struct ext4_block_allocator *alloc,
                           struct device *dev, struct ext4_inode *inode,
                           const struct file_entry *fe,
                           const struct chunk_map *chunk_map,
                           const struct ext4_layout *layout);
/* Journal writer — creates JBD2 journal (inode 8) */
int ext4_write_journal(struct device *dev, const struct ext4_layout *layout,
                       struct ext4_block_allocator *alloc,
                       uint64_t device_size);
int ext4_finalize_journal_inode(struct device *dev,
                                const struct ext4_layout *layout);
uint64_t ext4_journal_start_block(void);
uint32_t ext4_journal_block_count(void);

#endif /* EXT4_WRITER_H */
