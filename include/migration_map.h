/*
 * migration_map.h — Persistent Block Relocation Journal (Phase 3 Approach C)
 */
#ifndef MIGRATION_MAP_H
#define MIGRATION_MAP_H
#include "relocator.h"
#include <stdint.h>
#define MIGRATION_MAGIC "B2E4MAP1"
#define MIGRATION_FOOTER_OFFSET 8192
#define SUPERBLOCK_BACKUP_OFFSET 4096
#define MIGRATION_MAX_ENTRIES (1024U * 1024U)
#define MIGRATION_DEFAULT_CHECKPOINT_INTERVAL 8
#define MIGRATION_CHECKPOINT_BYTES (32ULL * 1024 * 1024)
struct migration_footer {
  char magic[8];
  uint64_t map_offset;
  uint32_t entry_count;
  uint32_t crc32;
  uint32_t checkpoint_interval;
  uint32_t padding[10];
};
struct device;
int migration_map_compute_layout(uint64_t dev_size, uint32_t entry_count,
    uint64_t *map_offset, uint64_t *footer_offset,
    uint64_t *backup_offset, uint64_t *map_size);
int migration_map_footer_present(struct device *dev);
int migration_map_detect_existing(struct device *dev);
uint32_t migration_map_completed_count(const struct relocation_plan *plan);
int migration_map_validate_plan(const struct relocation_entry *entries,
    uint32_t count, uint64_t dev_size);
int migration_map_preflight_space(struct device *dev, uint32_t entry_count);
int migration_map_save(struct device *dev, const struct relocation_plan *plan);
int migration_map_load(struct device *dev, struct relocation_plan *plan);
int migration_map_flush_progress(struct device *dev,
    const struct relocation_plan *plan);
int migration_map_rollback(struct device *dev);
#endif
