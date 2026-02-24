/*
 * ext4_structures.h — Ext4 on-disk format structures
 *
 * Packed C structs matching the ext4 on-disk format.
 * References:
 *   - https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
 *   - linux/fs/ext4/ext4.h
 *
 * All multi-byte fields are little-endian.
 */

#ifndef EXT4_STRUCTURES_H
#define EXT4_STRUCTURES_H

#include <stdint.h>

/* ========================================================================
 * Magic and constants
 * ======================================================================== */

#define EXT4_SUPER_MAGIC 0xEF53
#define EXT4_SUPER_OFFSET 1024 /* superblock starts at byte 1024 */
#define EXT4_MIN_BLOCK_SIZE 1024
#define EXT4_MAX_BLOCK_SIZE 65536
#define EXT4_DEFAULT_BLOCK_SIZE 4096

#define EXT4_GOOD_OLD_INODE_SIZE 128
#define EXT4_DEFAULT_INODE_SIZE 256
#define EXT4_DEFAULT_INODE_RATIO 16384

#define EXT4_ROOT_INO 2
#define EXT4_UNDEL_DIR_INO 6
#define EXT4_RESIZE_INO 7
#define EXT4_JOURNAL_INO 8
#define EXT4_GOOD_OLD_FIRST_INO 11

#define EXT4_LABEL_MAX 16

/* Superblock state */
#define EXT4_VALID_FS 0x0001
#define EXT4_ERROR_FS 0x0002
#define EXT4_ORPHAN_FS 0x0004

/* Superblock error handling */
#define EXT4_ERRORS_CONTINUE 1
#define EXT4_ERRORS_RO 2
#define EXT4_ERRORS_PANIC 3

/* Creator OS */
#define EXT4_OS_LINUX 0
#define EXT4_OS_HURD 1
#define EXT4_OS_MASIX 2
#define EXT4_OS_FREEBSD 3
#define EXT4_OS_LITES 4

/* Revision levels */
#define EXT4_GOOD_OLD_REV 0
#define EXT4_DYNAMIC_REV 1

/* Compatible feature flags (s_feature_compat) */
#define EXT4_FEATURE_COMPAT_DIR_PREALLOC 0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR 0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INODE 0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX 0x0020
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200

/* Incompatible feature flags (s_feature_incompat) */
#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x0004
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG 0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080
#define EXT4_FEATURE_INCOMPAT_MMP 0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000

/* Read-only compatible feature flags (s_feature_ro_compat) */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE 0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR 0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE 0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM 0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK 0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400

/* Default hash algorithm */
#define EXT4_HASH_HALF_MD4 1
#define EXT4_HASH_TEA 2

/* Inode flags */
#define EXT4_SECRM_FL 0x00000001
#define EXT4_UNRM_FL 0x00000002
#define EXT4_COMPR_FL 0x00000004
#define EXT4_SYNC_FL 0x00000008
#define EXT4_IMMUTABLE_FL 0x00000010
#define EXT4_APPEND_FL 0x00000020
#define EXT4_NODUMP_FL 0x00000040
#define EXT4_NOATIME_FL 0x00000080
#define EXT4_INDEX_FL 0x00001000 /* hash-indexed directory */
#define EXT4_JOURNAL_DATA_FL 0x00004000
#define EXT4_EXTENTS_FL 0x00080000 /* inode uses extents */
#define EXT4_INLINE_DATA_FL 0x10000000

/* File type for directory entries */
#define EXT4_FT_UNKNOWN 0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR 2
#define EXT4_FT_CHRDEV 3
#define EXT4_FT_BLKDEV 4
#define EXT4_FT_FIFO 5
#define EXT4_FT_SOCK 6
#define EXT4_FT_SYMLINK 7

/* Extent tree magic */
#define EXT4_EXT_MAGIC 0xF30A

/* ========================================================================
 * Superblock (at offset 1024, size = 1024 bytes)
 * ======================================================================== */

struct ext4_super_block {
  uint32_t s_inodes_count;         /* total inode count */
  uint32_t s_blocks_count_lo;      /* total block count (lo 32) */
  uint32_t s_r_blocks_count_lo;    /* reserved block count (lo 32) */
  uint32_t s_free_blocks_count_lo; /* free block count (lo 32) */
  uint32_t s_free_inodes_count;    /* free inode count */
  uint32_t s_first_data_block;     /* first data block (0 or 1) */
  uint32_t s_log_block_size;       /* block size = 1024 << s_log_block_size */
  uint32_t s_log_cluster_size;     /* cluster size (usually same as block) */
  uint32_t s_blocks_per_group;     /* blocks per block group */
  uint32_t s_clusters_per_group;   /* clusters per block group */
  uint32_t s_inodes_per_group;     /* inodes per block group */
  uint32_t s_mtime;                /* mount time */
  uint32_t s_wtime;                /* write time */
  uint16_t s_mnt_count;            /* mount count since last fsck */
  uint16_t s_max_mnt_count;        /* max mounts before fsck */
  uint16_t s_magic;                /* EXT4_SUPER_MAGIC (0xEF53) */
  uint16_t s_state;                /* filesystem state */
  uint16_t s_errors;               /* error handling behavior */
  uint16_t s_minor_rev_level;      /* minor revision level */
  uint32_t s_lastcheck;            /* time of last check */
  uint32_t s_checkinterval;        /* max time between checks */
  uint32_t s_creator_os;           /* OS that created the fs */
  uint32_t s_rev_level;            /* revision level */
  uint16_t s_def_resuid;           /* default uid for reserved blocks */
  uint16_t s_def_resgid;           /* default gid for reserved blocks */

  /* Dynamic revision (rev >= 1) fields */
  uint32_t s_first_ino;      /* first usable inode (usually 11) */
  uint16_t s_inode_size;     /* inode size (usually 256) */
  uint16_t s_block_group_nr; /* block group # of this superblock */
  uint32_t s_feature_compat;
  uint32_t s_feature_incompat;
  uint32_t s_feature_ro_compat;
  uint8_t s_uuid[16]; /* filesystem UUID */
  char s_volume_name[EXT4_LABEL_MAX];
  char s_last_mounted[64];
  uint32_t s_algorithm_usage_bitmap;

  /* Performance hints */
  uint8_t s_prealloc_blocks;
  uint8_t s_prealloc_dir_blocks;
  uint16_t s_reserved_gdt_blocks; /* reserved GDT blocks for expansion */

  /* Journalling support (if COMPAT_HAS_JOURNAL) */
  uint8_t s_journal_uuid[16];
  uint32_t s_journal_inum;
  uint32_t s_journal_dev;
  uint32_t s_last_orphan;
  uint32_t s_hash_seed[4];
  uint8_t s_def_hash_version;
  uint8_t s_jnl_backup_type;
  uint16_t s_desc_size; /* group descriptor size (usually 64) */
  uint32_t s_default_mount_opts;
  uint32_t s_first_meta_bg;
  uint32_t s_mkfs_time;
  uint32_t s_jnl_blocks[17];

  /* 64-bit support */
  uint32_t s_blocks_count_hi;
  uint32_t s_r_blocks_count_hi;
  uint32_t s_free_blocks_count_hi;
  uint16_t s_min_extra_isize;
  uint16_t s_want_extra_isize;
  uint32_t s_flags;
  uint16_t s_raid_stride;
  uint16_t s_mmp_interval;
  uint64_t s_mmp_block;
  uint32_t s_raid_stripe_width;
  uint8_t s_log_groups_per_flex;
  uint8_t s_checksum_type;
  uint16_t s_reserved_pad;
  uint64_t s_kbytes_written;

  /* Snapshot support (unused by us) */
  uint32_t s_snapshot_inum;
  uint32_t s_snapshot_id;
  uint64_t s_snapshot_r_blocks_count;
  uint32_t s_snapshot_list;

  /* Error tracking */
  uint32_t s_error_count;
  uint32_t s_first_error_time;
  uint32_t s_first_error_ino;
  uint64_t s_first_error_block;
  uint8_t s_first_error_func[32];
  uint32_t s_first_error_line;
  uint32_t s_last_error_time;
  uint32_t s_last_error_ino;
  uint32_t s_last_error_line;
  uint64_t s_last_error_block;
  uint8_t s_last_error_func[32];

  uint8_t s_mount_opts[64];

  /* Metadata checksum seed */
  uint32_t s_usr_quota_inum;
  uint32_t s_grp_quota_inum;
  uint32_t s_overhead_blocks;
  uint32_t s_backup_bgs[2];
  uint8_t s_encrypt_algos[4];
  uint8_t s_encrypt_pw_salt[16];
  uint32_t s_lpf_ino;
  uint32_t s_prj_quota_inum;
  uint32_t s_checksum_seed;

  uint32_t s_reserved[98];
  uint32_t s_checksum; /* crc32c of superblock */
} __attribute__((packed));

/* ========================================================================
 * Block Group Descriptor (64 bytes with 64bit feature)
 * ======================================================================== */

struct ext4_group_desc {
  uint32_t bg_block_bitmap_lo;
  uint32_t bg_inode_bitmap_lo;
  uint32_t bg_inode_table_lo;
  uint16_t bg_free_blocks_count_lo;
  uint16_t bg_free_inodes_count_lo;
  uint16_t bg_used_dirs_count_lo;
  uint16_t bg_flags;
  uint32_t bg_exclude_bitmap_lo;
  uint16_t bg_block_bitmap_csum_lo;
  uint16_t bg_inode_bitmap_csum_lo;
  uint16_t bg_itable_unused_lo;
  uint16_t bg_checksum;

  /* 64-bit fields */
  uint32_t bg_block_bitmap_hi;
  uint32_t bg_inode_bitmap_hi;
  uint32_t bg_inode_table_hi;
  uint16_t bg_free_blocks_count_hi;
  uint16_t bg_free_inodes_count_hi;
  uint16_t bg_used_dirs_count_hi;
  uint16_t bg_itable_unused_hi;
  uint32_t bg_exclude_bitmap_hi;
  uint16_t bg_block_bitmap_csum_hi;
  uint16_t bg_inode_bitmap_csum_hi;
  uint32_t bg_reserved;
} __attribute__((packed));

/* ========================================================================
 * Inode (256 bytes default, 128 bytes for "good old" ext2)
 * ======================================================================== */

struct ext4_inode {
  uint16_t i_mode;
  uint16_t i_uid;
  uint32_t i_size_lo;
  uint32_t i_atime;
  uint32_t i_ctime;
  uint32_t i_mtime;
  uint32_t i_dtime;
  uint16_t i_gid;
  uint16_t i_links_count;
  uint32_t i_blocks_lo; /* 512-byte sectors, not fs blocks */
  uint32_t i_flags;
  uint32_t i_osd1; /* OS dependent */

  uint8_t i_block[60]; /* 15 x 4-byte block ptrs, OR extent tree header */

  uint32_t i_generation;
  uint32_t i_file_acl_lo; /* extended attribute block */
  uint32_t i_size_high;   /* high 32 bits of size (for files >= 2GB) */
  uint32_t i_obso_faddr;  /* obsolete */

  /* OS dependent 2 */
  uint16_t i_blocks_high;
  uint16_t i_file_acl_high;
  uint16_t i_uid_high;
  uint16_t i_gid_high;
  uint16_t i_checksum_lo;
  uint16_t i_reserved;

  /* Extra fields (if inode_size > 128) */
  uint16_t i_extra_isize;
  uint16_t i_checksum_hi;
  uint32_t i_ctime_extra; /* extra time precision (nanoseconds) */
  uint32_t i_mtime_extra;
  uint32_t i_atime_extra;
  uint32_t i_crtime; /* creation time */
  uint32_t i_crtime_extra;
  uint32_t i_version_hi;
  uint32_t i_projid;
} __attribute__((packed));

/* ========================================================================
 * Extent tree structures (stored in inode.i_block for extents-enabled inodes)
 * ======================================================================== */

struct ext4_extent_header {
  uint16_t eh_magic;      /* EXT4_EXT_MAGIC (0xF30A) */
  uint16_t eh_entries;    /* number of valid entries */
  uint16_t eh_max;        /* max number of entries */
  uint16_t eh_depth;      /* depth of tree (0 = leaf) */
  uint32_t eh_generation; /* generation of tree */
} __attribute__((packed));

/* Leaf extent (eh_depth == 0) */
struct ext4_extent {
  uint32_t ee_block;    /* first file block covered */
  uint16_t ee_len;      /* number of blocks covered */
  uint16_t ee_start_hi; /* physical block (hi 16 bits) */
  uint32_t ee_start_lo; /* physical block (lo 32 bits) */
} __attribute__((packed));

/* Index extent (eh_depth > 0) */
struct ext4_extent_idx {
  uint32_t ei_block;   /* covers file blocks from this block */
  uint32_t ei_leaf_lo; /* lower 32-bits of block of next level */
  uint16_t ei_leaf_hi; /* upper 16-bits of block of next level */
  uint16_t ei_unused;
} __attribute__((packed));

/* Tail checksum (at end of extent block) */
struct ext4_extent_tail {
  uint32_t et_checksum;
} __attribute__((packed));

/* ========================================================================
 * Extended Attributes
 * ======================================================================== */
#define EXT4_XATTR_MAGIC 0xEA020000
#define EXT4_XATTR_INDEX_SYSTEM 3
#define EXT4_XATTR_INDEX_SECURITY 6

struct ext4_xattr_ibody_header {
  uint32_t h_magic;
} __attribute__((packed));

struct ext4_xattr_header {
  uint32_t h_magic;
  uint32_t h_refcount;
  uint32_t h_blocks;
  uint32_t h_hash;
  uint32_t h_checksum;
  uint32_t h_reserved[3];
} __attribute__((packed));

struct ext4_xattr_entry {
  uint8_t e_name_len;
  uint8_t e_name_index;
  uint16_t e_value_offs;
  uint32_t e_value_block;
  uint32_t e_value_size;
  uint32_t e_hash;
  char e_name[];
} __attribute__((packed));

/* ========================================================================
 * Directory entry (variable length)
 * ======================================================================== */

struct ext4_dir_entry_2 {
  uint32_t inode;    /* inode number */
  uint16_t rec_len;  /* directory entry length */
  uint8_t name_len;  /* name length */
  uint8_t file_type; /* EXT4_FT_* */
  char name[255];    /* file name (NOT null-terminated on disk) */
} __attribute__((packed));

/* ========================================================================
 * HTree Directory Index structures
 * ======================================================================== */

struct ext4_dx_root_info {
  uint32_t reserved_zero;
  uint8_t hash_version; /* EXT4_HASH_* */
  uint8_t info_length;  /* 8 */
  uint8_t indirect_levels;
  uint8_t unused_flags;
} __attribute__((packed));

struct ext4_dx_entry {
  uint32_t hash;
  uint32_t block; /* block number in dir's logical space */
} __attribute__((packed));

struct ext4_dx_countlimit {
  uint16_t limit;
  uint16_t count;
} __attribute__((packed));

/* ========================================================================
 * Helper macros
 * ======================================================================== */

/* Is this block group number a superblock backup location? (sparse_super) */
static inline int ext4_bg_has_super(uint32_t group) {
  if (group == 0)
    return 1;
  if (group == 1)
    return 1;

  /* Check if group is a power of 3, 5, or 7 */
  uint32_t g;
  for (g = 3; g <= group; g *= 3)
    if (g == group)
      return 1;
  for (g = 5; g <= group; g *= 5)
    if (g == group)
      return 1;
  for (g = 7; g <= group; g *= 7)
    if (g == group)
      return 1;

  return 0;
}

#endif /* EXT4_STRUCTURES_H */
