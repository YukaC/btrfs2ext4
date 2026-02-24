/*
 * btrfs_reader.h — Btrfs filesystem reader API
 *
 * High-level API for reading all metadata from a btrfs filesystem.
 * Builds an in-memory representation of the filesystem tree.
 */

#ifndef BTRFS_READER_H
#define BTRFS_READER_H

#include "btrfs/btrfs_structures.h"
#include <stddef.h>
#include <stdint.h>

/* Maximum filename length */
#define BTRFS_MAX_NAME_LEN 255

/* Maximum number of extents per file (can grow dynamically) */
#define BTRFS_INITIAL_EXTENTS_CAPACITY 8

/* BTRFS key types for xattr parsing */

/* ========================================================================
 * In-memory file extent
 * ======================================================================== */

struct file_extent {
  uint64_t file_offset;    /* offset within the file */
  uint64_t disk_bytenr;    /* physical byte address on disk (0=hole) */
  uint64_t disk_num_bytes; /* size on disk */
  uint64_t num_bytes;      /* logical bytes in file */
  uint64_t ram_bytes;      /* decompressed size */
  uint8_t compression;     /* BTRFS_COMPRESS_* */
  uint8_t type;            /* BTRFS_FILE_EXTENT_INLINE/REG/PREALLOC */
  /* For inline extents, data is stored separately */
  uint8_t *inline_data;
  uint32_t inline_data_len;
};

/* ========================================================================
 * Extended attribute entry (for Phase 6: xattr/ACL preservation)
 * ======================================================================== */

struct xattr_entry {
  struct xattr_entry *next; /* linked list */
  char *name;               /* e.g. "security.capability" */
  void *value;
  uint16_t name_len;
  uint32_t value_len;
};

/* ========================================================================
 * In-memory file/directory entry
 * ======================================================================== */

struct file_entry;

struct dir_entry_link {
  struct file_entry *target;         /* the inode this dirent points to */
  char name[BTRFS_MAX_NAME_LEN + 1]; /* name of the link within its directory */
  uint16_t name_len;
};

struct file_entry {
  uint64_t ino;        /* btrfs objectid (inode number) */
  uint64_t parent_ino; /* primary parent directory for `..` linkage */

  /* Inode metadata */
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;
  uint32_t nlink;
  uint64_t size;
  uint64_t rdev; /* device number for chr/blk devices */

  /* Timestamps */
  int64_t atime_sec;
  uint32_t atime_nsec;
  int64_t mtime_sec;
  uint32_t mtime_nsec;
  int64_t ctime_sec;
  uint32_t ctime_nsec;
  int64_t crtime_sec; /* creation time (btrfs otime) */
  uint32_t crtime_nsec;

  /* Symlink target (if S_ISLNK) */
  char *symlink_target;

  /* File extents (for regular files) */
  struct file_extent *extents;
  uint32_t extent_count;
  uint32_t extent_capacity;

  /* Directory children (for directories) */
  struct dir_entry_link *children;
  uint32_t child_count;
  uint32_t child_capacity;

  /* Extended attributes (linked list) */
  struct xattr_entry *xattrs;

  /* Ext4 transient builder flags */
  uint32_t ext4_flags;
};

/* ========================================================================
 * In-memory used-block map (extent tree data)
 * ======================================================================== */

struct used_extent {
  uint64_t start;  /* physical byte offset */
  uint64_t length; /* length in bytes */
  uint64_t flags;  /* BTRFS_BLOCK_GROUP_DATA/METADATA/SYSTEM */
};

struct used_block_map {
  struct used_extent *extents;
  uint32_t count;
  uint32_t capacity;
};

/* ========================================================================
 * Inode lookup hash table (optional accelerator)
 * ======================================================================== */

struct inode_lookup_ht {
  struct file_entry **buckets;
  uint32_t capacity;
  uint32_t count;
};

/* ========================================================================
 * Complete btrfs filesystem representation
 * ======================================================================== */

struct btrfs_fs_info {
  /* Parsed superblock */
  struct btrfs_super_block sb;

  /* Device handle */
  struct device *dev;

  /* Chunk tree (logical → physical mapping) */
  struct chunk_map *chunk_map;

  /* Root filesystem tree */
  struct file_entry *root_dir;

  /* All inodes indexed by objectid (hash table or sorted array) */
  struct file_entry **inode_table;
  uint32_t inode_count;
  uint32_t inode_capacity;

  /* Used block map from extent tree */
  struct used_block_map used_blocks;

  /* Optional inode lookup hash table (accelerates btrfs_find_inode) */
  struct inode_lookup_ht ino_ht;

  /* Compression statistics (computed during Pass 1) */
  uint64_t total_compressed_bytes;   /* sum of disk_num_bytes for compressed */
  uint64_t total_decompressed_bytes; /* sum of ram_bytes for compressed */
  uint32_t compressed_extent_count;  /* number of compressed extents */

  /* CoW de-duplication statistics (Phase 4) */
  uint64_t dedup_blocks_needed; /* total extra blocks needed for CoW cloning */
  uint32_t shared_extent_count; /* number of shared (reflinked) extents */
};

/* ========================================================================
 * Adaptive Memory Configuration (production-grade hardware-agnostic)
 * ======================================================================== */

struct adaptive_mem_config {
  uint64_t total_ram;      /* physical RAM detected via sysconf */
  uint64_t available_ram;  /* currently available RAM */
  uint64_t mmap_threshold; /* dynamic threshold (60% of total_ram) */
  const char *workdir;     /* --workdir path for temp files */
  int workdir_is_tmpfs;    /* 1 = workdir is on tmpfs (WARNING) */
};

/* ========================================================================
 * Bloom filter for HDD thrashing prevention (graceful degradation)
 * ======================================================================== */

struct bloom_filter {
  uint8_t *bits;
  uint64_t size_bits;  /* total number of bits */
  uint32_t num_hashes; /* number of hash functions (k) */
};

int bloom_init(struct bloom_filter *bf, uint64_t expected_items);
void bloom_add(struct bloom_filter *bf, uint64_t key);
int bloom_test(const struct bloom_filter *bf, uint64_t key);
void bloom_free(struct bloom_filter *bf);

/* ========================================================================
 * Public API
 * ======================================================================== */

/*
 * Read all btrfs metadata from a device.
 * Populates fs_info with the complete filesystem state.
 * The device must already be opened with device_open().
 * Returns 0 on success, -1 on error.
 */
int btrfs_read_fs(struct device *dev, struct btrfs_fs_info *fs_info);

/*
 * Free all memory allocated by btrfs_read_fs().
 */
void btrfs_free_fs(struct btrfs_fs_info *fs_info);

/*
 * Find a file_entry by its btrfs inode number.
 * Returns NULL if not found.
 */
struct file_entry *btrfs_find_inode(struct btrfs_fs_info *fs_info,
                                    uint64_t ino);

#endif /* BTRFS_READER_H */
