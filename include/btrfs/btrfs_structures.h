/*
 * btrfs_structures.h — Btrfs on-disk format structures
 *
 * These packed structs match the Linux kernel's btrfs on-disk format exactly.
 * References:
 *   - https://btrfs.readthedocs.io/en/latest/dev/On-disk-format.html
 *   - linux/fs/btrfs/ctree.h
 *
 * All multi-byte fields are little-endian.
 */

#ifndef BTRFS_STRUCTURES_H
#define BTRFS_STRUCTURES_H

#include <stdint.h>

/* ========================================================================
 * Magic and constants
 * ======================================================================== */

#define BTRFS_MAGIC 0x4D5F53665248425FULL    /* "_BHRfS_M" LE */
#define BTRFS_SUPER_OFFSET 0x10000           /* 64 KiB */
#define BTRFS_SUPER_MIRROR_1 0x4000000       /* 64 MiB */
#define BTRFS_SUPER_MIRROR_2 0x4000000000ULL /* 256 GiB */

#define BTRFS_CSUM_SIZE 32
#define BTRFS_FSID_SIZE 16
#define BTRFS_UUID_SIZE 16
#define BTRFS_LABEL_SIZE 256
#define BTRFS_SYSTEM_CHUNK_ARRAY_SIZE 2048
#define BTRFS_NUM_BACKUP_ROOTS 4

#define BTRFS_MAX_LEVEL 8

/* Checksum types */
#define BTRFS_CSUM_TYPE_CRC32 0
#define BTRFS_CSUM_TYPE_XXHASH 1
#define BTRFS_CSUM_TYPE_SHA256 2
#define BTRFS_CSUM_TYPE_BLAKE2 3

/* Object types (key.type field) */
#define BTRFS_INODE_ITEM_KEY 0x01
#define BTRFS_INODE_REF_KEY 0x0C
#define BTRFS_INODE_EXTREF_KEY 0x0D
#define BTRFS_XATTR_ITEM_KEY 0x18
#define BTRFS_ORPHAN_ITEM_KEY 0x30
#define BTRFS_DIR_LOG_ITEM_KEY 0x3C
#define BTRFS_DIR_LOG_INDEX_KEY 0x48
#define BTRFS_DIR_ITEM_KEY 0x54
#define BTRFS_DIR_INDEX_KEY 0x60
#define BTRFS_EXTENT_DATA_KEY 0x6C
#define BTRFS_EXTENT_CSUM_KEY 0x80
#define BTRFS_ROOT_ITEM_KEY 0x84
#define BTRFS_ROOT_BACKREF_KEY 0x90
#define BTRFS_ROOT_REF_KEY 0x9C
#define BTRFS_EXTENT_ITEM_KEY 0xA8
#define BTRFS_METADATA_ITEM_KEY 0xA9
#define BTRFS_TREE_BLOCK_REF_KEY 0xB0
#define BTRFS_EXTENT_DATA_REF_KEY 0xB2
#define BTRFS_SHARED_BLOCK_REF_KEY 0xB6
#define BTRFS_SHARED_DATA_REF_KEY 0xB8
#define BTRFS_BLOCK_GROUP_ITEM_KEY 0xC0
#define BTRFS_DEV_EXTENT_KEY 0xCC
#define BTRFS_DEV_ITEM_KEY 0xD8
#define BTRFS_CHUNK_ITEM_KEY 0xE4
#define BTRFS_STRING_ITEM_KEY 0xFD

/* Well-known object IDs */
#define BTRFS_ROOT_TREE_OBJECTID 1ULL
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL
#define BTRFS_CHUNK_TREE_OBJECTID 3ULL
#define BTRFS_DEV_TREE_OBJECTID 4ULL
#define BTRFS_FS_TREE_OBJECTID 5ULL
#define BTRFS_ROOT_TREE_DIR_OBJECTID 6ULL
#define BTRFS_CSUM_TREE_OBJECTID 7ULL
#define BTRFS_FIRST_FREE_OBJECTID 256ULL
#define BTRFS_LAST_FREE_OBJECTID 0xFFFFFFFFFFFFFF00ULL
#define BTRFS_FIRST_CHUNK_TREE_OBJECTID 256ULL

/* File extent types */
#define BTRFS_FILE_EXTENT_INLINE 0
#define BTRFS_FILE_EXTENT_REG 1
#define BTRFS_FILE_EXTENT_PREALLOC 2

/* Compression types */
#define BTRFS_COMPRESS_NONE 0
#define BTRFS_COMPRESS_ZLIB 1
#define BTRFS_COMPRESS_LZO 2
#define BTRFS_COMPRESS_ZSTD 3

/* Directory item types (matches DT_* from dirent.h mostly) */
#define BTRFS_FT_UNKNOWN 0
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR 2
#define BTRFS_FT_CHRDEV 3
#define BTRFS_FT_BLKDEV 4
#define BTRFS_FT_FIFO 5
#define BTRFS_FT_SOCK 6
#define BTRFS_FT_SYMLINK 7
#define BTRFS_FT_XATTR 8

/* Block group flags */
#define BTRFS_BLOCK_GROUP_DATA (1ULL << 0)
#define BTRFS_BLOCK_GROUP_SYSTEM (1ULL << 1)
#define BTRFS_BLOCK_GROUP_METADATA (1ULL << 2)

/* ========================================================================
 * On-disk key structure (17 bytes)
 * ======================================================================== */

struct btrfs_disk_key {
  uint64_t objectid;
  uint8_t type;
  uint64_t offset;
} __attribute__((packed));

/* ========================================================================
 * Btrfs timestamp
 * ======================================================================== */

struct btrfs_timespec {
  int64_t sec;
  uint32_t nsec;
} __attribute__((packed));

/* ========================================================================
 * Device item (within superblock and dev tree)
 * ======================================================================== */

struct btrfs_dev_item {
  uint64_t devid;
  uint64_t total_bytes;
  uint64_t bytes_used;
  uint32_t io_align;
  uint32_t io_width;
  uint32_t sector_size;
  uint64_t type;
  uint64_t generation;
  uint64_t start_offset;
  uint32_t dev_group;
  uint8_t seek_speed;
  uint8_t bandwidth;
  uint8_t uuid[BTRFS_UUID_SIZE];
  uint8_t fsid[BTRFS_FSID_SIZE];
} __attribute__((packed));

/* ========================================================================
 * Root backup (4 copies stored in superblock)
 * ======================================================================== */

struct btrfs_root_backup {
  uint64_t tree_root;
  uint64_t tree_root_gen;
  uint64_t chunk_root;
  uint64_t chunk_root_gen;
  uint64_t extent_root;
  uint64_t extent_root_gen;
  uint64_t fs_root;
  uint64_t fs_root_gen;
  uint64_t dev_root;
  uint64_t dev_root_gen;
  uint64_t csum_root;
  uint64_t csum_root_gen;
  uint64_t total_bytes;
  uint64_t bytes_used;
  uint64_t num_devices;
  uint64_t unused_64[4];
  uint8_t tree_root_level;
  uint8_t chunk_root_level;
  uint8_t extent_root_level;
  uint8_t fs_root_level;
  uint8_t dev_root_level;
  uint8_t csum_root_level;
  uint8_t unused_8[10];
} __attribute__((packed));

/* ========================================================================
 * Superblock (at physical offset 0x10000, size = 4096 bytes)
 * ======================================================================== */

struct btrfs_super_block {
  uint8_t csum[BTRFS_CSUM_SIZE];
  uint8_t fsid[BTRFS_FSID_SIZE];
  uint64_t bytenr; /* physical addr of this superblock */
  uint64_t flags;
  uint64_t magic; /* BTRFS_MAGIC */
  uint64_t generation;
  uint64_t root;       /* logical addr of root tree root */
  uint64_t chunk_root; /* logical addr of chunk tree root */
  uint64_t log_root;   /* logical addr of log tree root */
  uint64_t log_root_transid;
  uint64_t total_bytes;
  uint64_t bytes_used;
  uint64_t root_dir_objectid; /* usually 6 */
  uint64_t num_devices;
  uint32_t sectorsize;
  uint32_t nodesize;
  uint32_t __unused_leafsize; /* same as nodesize now */
  uint32_t stripesize;
  uint32_t sys_chunk_array_size;
  uint64_t chunk_root_generation;
  uint64_t compat_flags;
  uint64_t compat_ro_flags;
  uint64_t incompat_flags;
  uint16_t csum_type;
  uint8_t root_level;
  uint8_t chunk_root_level;
  uint8_t log_root_level;
  struct btrfs_dev_item dev_item;
  char label[BTRFS_LABEL_SIZE];
  uint64_t cache_generation;
  uint64_t uuid_tree_generation;
  uint8_t metadata_uuid[BTRFS_FSID_SIZE];
  /* future expansion ... */
  uint64_t reserved[28];
  uint8_t sys_chunk_array[BTRFS_SYSTEM_CHUNK_ARRAY_SIZE];
  struct btrfs_root_backup super_roots[BTRFS_NUM_BACKUP_ROOTS];
  /* padding to 4096 bytes happens naturally */
} __attribute__((packed));

/* ========================================================================
 * Node header (common to internal and leaf nodes)
 * ======================================================================== */

struct btrfs_header {
  uint8_t csum[BTRFS_CSUM_SIZE];
  uint8_t fsid[BTRFS_FSID_SIZE];
  uint64_t bytenr; /* logical address of this node */
  uint64_t flags;
  uint8_t chunk_tree_uuid[BTRFS_UUID_SIZE];
  uint64_t generation;
  uint64_t owner;   /* tree that owns this node */
  uint32_t nritems; /* number of items/key ptrs */
  uint8_t level;    /* 0 = leaf */
} __attribute__((packed));

/* ========================================================================
 * Key pointer (in internal nodes, follows header)
 * ======================================================================== */

struct btrfs_key_ptr {
  struct btrfs_disk_key key;
  uint64_t blockptr; /* logical address of child node */
  uint64_t generation;
} __attribute__((packed));

/* ========================================================================
 * Item (in leaf nodes, follows header)
 * ======================================================================== */

struct btrfs_item {
  struct btrfs_disk_key key;
  uint32_t offset; /* data offset relative to end of header */
  uint32_t size;   /* data size */
} __attribute__((packed));

/* ========================================================================
 * Chunk item + stripe (in chunk tree and sys_chunk_array)
 * ======================================================================== */

struct btrfs_stripe {
  uint64_t devid;
  uint64_t offset; /* physical offset on device */
  uint8_t dev_uuid[BTRFS_UUID_SIZE];
} __attribute__((packed));

struct btrfs_chunk {
  uint64_t length;
  uint64_t owner; /* always BTRFS_EXTENT_TREE_OBJECTID for data */
  uint64_t stripe_len;
  uint64_t type; /* BLOCK_GROUP_DATA/SYSTEM/METADATA flags */
  uint32_t io_align;
  uint32_t io_width;
  uint32_t sector_size;
  uint16_t num_stripes;
  uint16_t sub_stripes;
  /* followed by num_stripes * btrfs_stripe */
} __attribute__((packed));

/* ========================================================================
 * Inode item (defined before root_item which embeds it)
 * ======================================================================== */

struct btrfs_inode_item {
  uint64_t generation;
  uint64_t transid;
  uint64_t size;
  uint64_t nbytes;
  uint64_t block_group;
  uint32_t nlink;
  uint32_t uid;
  uint32_t gid;
  uint32_t mode;
  uint64_t rdev;
  uint64_t flags;
  uint64_t sequence;
  uint64_t reserved[4];
  struct btrfs_timespec atime;
  struct btrfs_timespec mtime;
  struct btrfs_timespec ctime;
  struct btrfs_timespec otime;
} __attribute__((packed));

/* ========================================================================
 * Root item (describes a tree in the root tree)
 * ======================================================================== */

struct btrfs_root_item {
  struct btrfs_inode_item inode;
  uint64_t generation;
  uint64_t root_dirid;
  uint64_t bytenr; /* logical addr of root node of this tree */
  uint64_t byte_limit;
  uint64_t bytes_used;
  uint64_t last_snapshot;
  uint64_t flags;
  uint32_t refs;
  struct btrfs_disk_key drop_progress;
  uint8_t drop_level;
  uint8_t level;
  uint64_t generation_v2;
  uint8_t uuid[BTRFS_UUID_SIZE];
  uint8_t parent_uuid[BTRFS_UUID_SIZE];
  uint8_t received_uuid[BTRFS_UUID_SIZE];
  uint64_t ctransid;
  uint64_t otransid;
  uint64_t stransid;
  uint64_t rtransid;
  struct btrfs_timespec ctime;
  struct btrfs_timespec otime;
  struct btrfs_timespec stime;
  struct btrfs_timespec rtime;
  uint64_t reserved[8];
} __attribute__((packed));

/* ========================================================================
 * Inode ref (filename -> parent dir mapping)
 * ======================================================================== */

struct btrfs_inode_ref {
  uint64_t index;
  uint16_t name_len;
  /* followed by name_len bytes of name */
} __attribute__((packed));

/* ========================================================================
 * Directory item / index
 * ======================================================================== */

struct btrfs_dir_item {
  struct btrfs_disk_key location;
  uint64_t transid;
  uint16_t data_len;
  uint16_t name_len;
  uint8_t type; /* BTRFS_FT_* */
  /* followed by name_len bytes of name, then data_len bytes of data */
} __attribute__((packed));

/* ========================================================================
 * File extent item
 * ======================================================================== */

struct btrfs_file_extent_item {
  uint64_t generation;
  uint64_t ram_bytes; /* decoded (decompressed) size */
  uint8_t compression;
  uint8_t encryption;
  uint16_t other_encoding;
  uint8_t type; /* BTRFS_FILE_EXTENT_INLINE/REG/PREALLOC */
  /*
   * For INLINE: the remaining item bytes ARE the data.
   * For REG/PREALLOC, the following fields are present:
   */
  uint64_t disk_bytenr;    /* logical address of extent (0 = sparse hole) */
  uint64_t disk_num_bytes; /* size of extent on disk */
  uint64_t offset;         /* offset within the decompressed extent */
  uint64_t num_bytes;      /* logical number of bytes in file */
} __attribute__((packed));

/* ========================================================================
 * Extent item (in extent tree - describes allocated extent)
 * ======================================================================== */

struct btrfs_extent_item {
  uint64_t refs;
  uint64_t generation;
  uint64_t flags;
} __attribute__((packed));

/* ========================================================================
 * Block group item
 * ======================================================================== */

struct btrfs_block_group_item {
  uint64_t used;
  uint64_t chunk_objectid;
  uint64_t flags; /* BTRFS_BLOCK_GROUP_DATA/SYSTEM/METADATA */
} __attribute__((packed));

#endif /* BTRFS_STRUCTURES_H */
