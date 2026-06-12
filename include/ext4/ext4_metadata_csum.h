/*
 * ext4_metadata_csum.h — Ext4 metadata checksum helpers (METADATA_CSUM)
 */

#ifndef EXT4_METADATA_CSUM_H
#define EXT4_METADATA_CSUM_H

#include <stddef.h>
#include <stdint.h>

#include "ext4/ext4_structures.h"

#define EXT4_CRC32C_CHKSUM 1

struct device;
struct ext4_layout;

uint32_t ext4_csum_seed_from_uuid(const uint8_t uuid[16]);
void ext4_superblock_csum_set(struct ext4_super_block *sb);
uint16_t ext4_group_desc_csum(uint32_t csum_seed, uint32_t group,
                              const struct ext4_group_desc *desc,
                              size_t desc_size);
void ext4_update_gdt_used_dirs(struct device *dev, struct ext4_layout *layout);

#endif /* EXT4_METADATA_CSUM_H */
