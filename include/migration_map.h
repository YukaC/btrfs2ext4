/*
 * migration_map.h — Persistent Block Relocation Journal
 */
#ifndef MIGRATION_MAP_H
#define MIGRATION_MAP_H
#include "relocator.h"
#include <stdint.h>
#define MIGRATION_MAGIC "B2E4MAP1"
#define MIGRATION_FOOTER_OFFSET 8192
#define SUPERBLOCK_BACKUP_OFFSET 4096
struct migration_footer {
  char magic[8];
  uint64_t map_offset;
  uint32_t entry_count;
  uint32_t crc32;
  uint32_t padding[11];
};
struct device;
int migration_map_save(struct device *dev, const struct relocation_plan *plan);
int migration_map_rollback(struct device *dev);
#endif
