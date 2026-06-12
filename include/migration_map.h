/*
 * migration_map.h — Persistent Block Relocation Journal (Phase 3 Approach A+)
 */
#ifndef MIGRATION_MAP_H
#define MIGRATION_MAP_H

#include "relocator.h"
#include <stdint.h>

#define MIGRATION_MAGIC "B2E4MAP1"
#define MIGRATION_FOOTER_OFFSET 8192
#define SUPERBLOCK_BACKUP_OFFSET 4096
#define MIGRATION_MAX_ENTRIES (1024U * 1024U)

#define MIGRATION_PHASE_IDLE 0U
#define MIGRATION_PHASE_PASS2_DONE 2U
#define MIGRATION_PHASE_PASS3 3U

#define PASS3_STEP_SUPERBLOCK 0x01U
#define PASS3_STEP_GDT 0x02U
#define PASS3_STEP_INODES 0x04U
#define PASS3_STEP_DIRS 0x08U
#define PASS3_STEP_JOURNAL 0x10U
#define PASS3_STEP_BITMAPS 0x20U
#define PASS3_STEP_FREE_COUNTS 0x40U
#define PASS3_STEP_COMPLETE 0x80U
#define PASS3_STEP_PARTIAL_MASK 0x7FU

struct migration_footer {
  char magic[8];
  uint64_t map_offset;
  uint32_t entry_count;
  uint32_t crc32;
  uint32_t phase;
  uint32_t pass3_step_mask;
  uint64_t timestamp;
  uint32_t padding[6];
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
int migration_map_update_entry(struct device *dev, uint32_t index,
                               const struct relocation_entry *entry);
int migration_map_rollback(struct device *dev);
int migration_map_read_footer(struct device *dev,
                              struct migration_footer *footer);
uint32_t migration_map_footer_effective_phase(
    const struct migration_footer *footer);
int migration_map_set_pass3_step(struct device *dev, uint32_t mask_bit);
int migration_map_wipe_footer(struct device *dev);
int migration_map_finalize_success(struct device *dev);
int migration_map_emergency_recover(struct device *dev);

#endif /* MIGRATION_MAP_H */
