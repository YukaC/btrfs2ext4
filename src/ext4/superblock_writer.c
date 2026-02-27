/*
 * superblock_writer.c — Ext4 superblock writer
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uuid/uuid.h>

#include "btrfs/btrfs_reader.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

/* CRC32C from superblock.c */
extern uint32_t crc32c(uint32_t crc, const void *buf, size_t len);

int ext4_write_superblock(struct device *dev, const struct ext4_layout *layout,
                          const struct btrfs_fs_info *fs_info) {
  struct ext4_super_block sb;
  memset(&sb, 0, sizeof(sb));

  uint32_t block_size = layout->block_size;
  uint32_t log_block_size = 0;
  uint32_t bs = block_size;
  while (bs > 1024) {
    bs >>= 1;
    log_block_size++;
  }

  time_t now = time(NULL);

  /* Basic fields */
  sb.s_inodes_count = htole32(layout->total_inodes);
  sb.s_blocks_count_lo = htole32((uint32_t)(layout->total_blocks & 0xFFFFFFFF));
  sb.s_blocks_count_hi = htole32((uint32_t)(layout->total_blocks >> 32));
  sb.s_r_blocks_count_lo =
      htole32((uint32_t)(layout->total_blocks / 20)); /* 5% reserved */
  sb.s_free_blocks_count_lo = htole32(0); /* Will be calculated later */
  sb.s_free_inodes_count = htole32(layout->total_inodes - fs_info->inode_count -
                                   EXT4_GOOD_OLD_FIRST_INO);
  sb.s_first_data_block = htole32(block_size > 1024 ? 0 : 1);
  sb.s_log_block_size = htole32(log_block_size);
  sb.s_log_cluster_size = htole32(log_block_size);
  sb.s_blocks_per_group = htole32(layout->blocks_per_group);
  sb.s_clusters_per_group = htole32(layout->blocks_per_group);
  sb.s_inodes_per_group = htole32(layout->inodes_per_group);
  sb.s_mtime = htole32((uint32_t)now);
  sb.s_wtime = htole32((uint32_t)now);
  sb.s_mnt_count = htole16(0);
  sb.s_max_mnt_count = htole16(0xFFFF); /* disable fsck-on-mount-count */
  sb.s_magic = htole16(EXT4_SUPER_MAGIC);
  sb.s_state = htole16(EXT4_VALID_FS);
  sb.s_errors = htole16(EXT4_ERRORS_CONTINUE);
  sb.s_minor_rev_level = htole16(0);
  sb.s_lastcheck = htole32((uint32_t)now);
  sb.s_checkinterval = htole32(0);
  sb.s_creator_os = htole32(EXT4_OS_LINUX);
  sb.s_rev_level = htole32(EXT4_DYNAMIC_REV);
  sb.s_def_resuid = htole16(0);
  sb.s_def_resgid = htole16(0);

  /* Dynamic revision fields */
  sb.s_first_ino = htole32(EXT4_GOOD_OLD_FIRST_INO);
  sb.s_inode_size = htole16((uint16_t)layout->inode_size);
  sb.s_block_group_nr = htole16(0);

  /* Feature flags:
   * - COMPAT: EXT_ATTR, DIR_INDEX, HAS_JOURNAL
   * - INCOMPAT: FILETYPE, EXTENTS, 64BIT, FLEX_BG
   * - RO_COMPAT: SPARSE_SUPER, LARGE_FILE, HUGE_FILE, GDT_CSUM,
   *              DIR_NLINK, EXTRA_ISIZE
   */
  sb.s_feature_compat = htole32(
      EXT4_FEATURE_COMPAT_EXT_ATTR | EXT4_FEATURE_COMPAT_DIR_INDEX |
      EXT4_FEATURE_COMPAT_RESIZE_INODE | EXT4_FEATURE_COMPAT_HAS_JOURNAL);
  /* Bug O fix: Added CSUM_SEED (incompat) and METADATA_CSUM (ro_compat)
   * for modern ext4 metadata checksumming support (kernel 3.18+). */
  sb.s_feature_incompat =
      htole32(EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS |
              EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG |
              EXT4_FEATURE_INCOMPAT_CSUM_SEED);
  sb.s_feature_ro_compat = htole32(
      EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
      EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM |
      EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
      EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);

  /* Generate UUID */
  uuid_generate(sb.s_uuid);

  /* Volume name — copy from btrfs label if available */
  if (fs_info->sb.label[0]) {
    strncpy(sb.s_volume_name, fs_info->sb.label, EXT4_LABEL_MAX - 1);
  }

  /* Hash seed for htree directories */
  uuid_generate((unsigned char *)sb.s_hash_seed);
  sb.s_def_hash_version = EXT4_HASH_HALF_MD4;

  /* Journal configuration */
  sb.s_journal_inum = htole32(EXT4_JOURNAL_INO);

  /* Group descriptor size (64 bytes for 64-bit mode) */
  sb.s_desc_size = htole16(layout->desc_size);

  /* mkfs time */
  sb.s_mkfs_time = htole32((uint32_t)now);

  /* Extra inode size */
  sb.s_min_extra_isize = htole16(32);
  sb.s_want_extra_isize = htole16(32);

  /* Flex block group size: 16 groups per flex */
  sb.s_log_groups_per_flex = 4; /* 2^4 = 16 */

  /* Reserved GDT blocks */
  sb.s_reserved_gdt_blocks =
      htole16((uint16_t)layout->groups[0].reserved_gdt_blocks);

  /* Write primary superblock at offset 1024 */
  printf("Writing ext4 superblock at offset %u...\n", EXT4_SUPER_OFFSET);

  /* Superblock needs to be padded to block_size for writing to block devices */
  uint8_t *sb_buf = calloc(1, block_size);
  if (!sb_buf)
    return -1;

  /* The superblock always starts at byte 1024, which may be within block 0 */
  memcpy(sb_buf + (EXT4_SUPER_OFFSET % block_size), &sb, sizeof(sb));

  uint64_t sb_block_offset = (EXT4_SUPER_OFFSET / block_size) * block_size;
  if (device_write(dev, sb_block_offset, sb_buf, block_size) < 0) {
    free(sb_buf);
    return -1;
  }

  /* Write backup copies to block groups with sparse_super */
  for (uint32_t g = 1; g < layout->num_groups; g++) {
    if (!layout->groups[g].has_super)
      continue;

    sb.s_block_group_nr = htole16((uint16_t)g);

    memset(sb_buf, 0, block_size);
    memcpy(sb_buf, &sb,
           sizeof(sb)); /* In backup groups, SB is at start of block */

    uint64_t backup_offset = layout->groups[g].superblock_block * block_size;
    if (device_write(dev, backup_offset, sb_buf, block_size) < 0) {
      free(sb_buf);
      return -1;
    }
  }

  free(sb_buf);
  printf("  Superblock written (+ %u backup copies)\n",
         layout->num_groups > 1 ? layout->num_groups - 1 : 0);

  return 0;
}
