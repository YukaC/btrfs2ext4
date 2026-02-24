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
  uint32_t padding[11]; /* pad to 64 bytes */
};

struct device;

/*
 * Saves the entire relocation plan and the original btrfs superblock
 * to the end of the device. This MUST be called before executing any block
 * relocations.
 *
 * Returns 0 on success, -1 on error.
 */
int migration_map_save(struct device *dev, const struct relocation_plan *plan);

/*
 * Performs a full rollback using the written migration map.
 * Reads the footer from the end of the device, reverses all physical block
 * copies (from dst_offset back to src_offset), and finally restores the
 * primary Btrfs superblock.
 *
 * It erases the migration footer when successfully completed to prevent
 * accidental double-rollbacks.
 *
 * Returns 0 on success, -1 on error.
 */
int migration_map_rollback(struct device *dev);

#endif /* MIGRATION_MAP_H */
