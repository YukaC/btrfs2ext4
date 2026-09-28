/*
 * metadata_csum.c — Ext4 METADATA_CSUM implementation
 */

#include <endian.h>
#include <stddef.h>
#include <string.h>

#include "btrfs/checksum.h"
#include "device_io.h"
#include "ext4/ext4_crc16.h"
#include "ext4/ext4_metadata_csum.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"

uint32_t ext4_csum_seed_from_uuid(const uint8_t uuid[16]) {
  return crc32c((uint32_t)~0, uuid, 16);
}

void ext4_superblock_csum_set(struct ext4_super_block *sb) {
  sb->s_checksum_type = EXT4_CRC32C_CHKSUM;
  sb->s_checksum = 0;
  sb->s_checksum =
      htole32(crc32c((uint32_t)~0, sb, offsetof(struct ext4_super_block, s_checksum)));
}

uint16_t ext4_group_desc_csum(uint32_t csum_seed, uint32_t group,
                              const struct ext4_group_desc *desc,
                              size_t desc_size) {
  uint32_t le_group = htole32(group);
  uint32_t crc = crc32c(csum_seed, &le_group, sizeof(le_group));

  uint8_t tmp[64];
  size_t n = desc_size < sizeof(tmp) ? desc_size : sizeof(tmp);
  memcpy(tmp, desc, n);
  if (n >= 32) {
    tmp[30] = 0;
    tmp[31] = 0;
  }

  crc = crc32c(crc, tmp, desc_size);
  return (uint16_t)(crc & 0xFFFF);
}

void ext4_update_gdt_used_dirs(struct device *dev, struct ext4_layout *layout) {
  uint32_t block_size = layout->block_size;

  struct ext4_super_block sb;
  if (device_read(dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) < 0)
    return;

  uint32_t ro_compat = le32toh(sb.s_feature_ro_compat);
  int metadata_csum = !!(ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);
  uint32_t csum_seed = le32toh(sb.s_checksum_seed);

  for (uint32_t g = 0; g < layout->num_groups; g++) {
    uint32_t used = layout->groups[g].used_dirs_count;
    if (used == 0)
      continue;

    uint64_t gdt_offset = layout->groups[0].gdt_start_block * block_size +
                          (uint64_t)g * layout->desc_size;

    uint8_t gd_buf[64];
    memset(gd_buf, 0, sizeof(gd_buf));
    if (device_read(dev, gdt_offset, gd_buf, layout->desc_size) < 0)
      continue;

    struct ext4_group_desc *desc = (struct ext4_group_desc *)gd_buf;
    desc->bg_used_dirs_count_lo = htole16((uint16_t)(used & 0xFFFF));
    desc->bg_used_dirs_count_hi = htole16((uint16_t)(used >> 16));
    desc->bg_checksum = 0;

    if (metadata_csum) {
      desc->bg_checksum =
          htole16(ext4_group_desc_csum(csum_seed, g, desc, layout->desc_size));
    } else {
      uint16_t crc = ext4_crc16((uint16_t)~0, sb.s_uuid, sizeof(sb.s_uuid));
      uint32_t le_group = htole32(g);
      crc = ext4_crc16(crc, &le_group, sizeof(le_group));
      crc = ext4_crc16(crc, desc, layout->desc_size);
      desc->bg_checksum = htole16(crc);
    }

    device_write(dev, gdt_offset, gd_buf, layout->desc_size);
  }
}
