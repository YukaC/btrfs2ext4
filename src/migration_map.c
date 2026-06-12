/*
 * migration_map.c — Persistent Block Relocation Journal
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/btrfs_structures.h"
#include "device_io.h"
#include "migration_map.h"
#include "relocator.h"

extern uint32_t crc32c(uint32_t crc, const void *buf, size_t len);

static const uint32_t MIGRATION_MAX_ENTRIES = 1024 * 1024;

static int migration_map_validate_footer_bounds(struct device *dev,
                                                const struct migration_footer *footer) {
  if (footer->entry_count > MIGRATION_MAX_ENTRIES) {
    fprintf(stderr,
            "btrfs2ext4: migration map entry_count %u exceeds max %u — "
            "refusing rollback\n",
            footer->entry_count, MIGRATION_MAX_ENTRIES);
    return -1;
  }

  uint64_t backup_offset = (dev->size - SUPERBLOCK_BACKUP_OFFSET) & ~4095ULL;
  uint64_t footer_offset = backup_offset - MIGRATION_FOOTER_OFFSET;

  if (footer->map_offset > dev->size)
    return -1;

  if (footer->entry_count > 0) {
    uint64_t map_size =
        (uint64_t)footer->entry_count * sizeof(struct relocation_entry);
    if (map_size > 1024ULL * 1024 * 1024)
      return -1;
    if (footer->map_offset + map_size > dev->size ||
        footer->map_offset + map_size > footer_offset)
      return -1;
  }
  return 0;
}

static int migration_map_validate_entry_bounds(struct device *dev,
                                               const struct relocation_entry *re) {
  if (re->length == 0)
    return -1;
  if (re->src_offset + re->length > dev->size ||
      re->dst_offset + re->length > dev->size)
    return -1;
  return 0;
}

int migration_map_save(struct device *dev, const struct relocation_plan *plan) {
  struct btrfs_super_block sb_backup;
  if (device_read(dev, BTRFS_SUPER_OFFSET, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  uint64_t backup_offset = (dev->size - SUPERBLOCK_BACKUP_OFFSET) & ~4095ULL;
  if (device_write(dev, backup_offset, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  uint64_t map_offset = 0;
  uint64_t map_size = 0;

  if (plan->count > 0) {
    if (plan->count > MIGRATION_MAX_ENTRIES)
      return -1;

    map_size = (uint64_t)plan->count * sizeof(struct relocation_entry);
    if (map_size > 1024ULL * 1024 * 1024)
      return -1;

    uint64_t footer_offset = backup_offset - MIGRATION_FOOTER_OFFSET;
    map_offset = ((footer_offset - map_size) + 4095ULL) & ~4095ULL;

    if (map_offset + map_size > footer_offset ||
        map_offset + map_size > dev->size)
      return -1;

    if (device_write(dev, map_offset, plan->entries, map_size) < 0)
      return -1;
  }

  struct migration_footer footer;
  memset(&footer, 0, sizeof(footer));
  memcpy(footer.magic, MIGRATION_MAGIC, 8);
  footer.map_offset = map_offset;
  footer.entry_count = plan->count;
  footer.crc32 =
      plan->count > 0 ? crc32c(0, plan->entries, map_size) : 0;

  uint64_t footer_offset = backup_offset - MIGRATION_FOOTER_OFFSET;
  if (device_write(dev, footer_offset, &footer, sizeof(footer)) < 0)
    return -1;

  if (device_sync(dev) < 0)
    return -1;
  return 0;
}

int migration_map_rollback(struct device *dev) {
  uint64_t backup_offset = (dev->size - SUPERBLOCK_BACKUP_OFFSET) & ~4095ULL;
  uint64_t footer_offset = backup_offset - MIGRATION_FOOTER_OFFSET;

  struct migration_footer footer;
  if (device_read(dev, footer_offset, &footer, sizeof(footer)) < 0)
    return -1;

  if (memcmp(footer.magic, MIGRATION_MAGIC, 8) != 0) {
    fprintf(stderr, "btrfs2ext4: no valid migration map found for rollback "
                    "(already rolled back or not converted)\n");
    return -1;
  }

  if (migration_map_validate_footer_bounds(dev, &footer) < 0)
    return -1;

  printf("Found valid migration map with %u entries.\n", footer.entry_count);

  if (footer.entry_count > 0) {
    uint64_t map_size =
        (uint64_t)footer.entry_count * sizeof(struct relocation_entry);
    struct relocation_entry *entries = malloc((size_t)map_size);
    if (!entries)
      return -1;

    if (device_read(dev, footer.map_offset, entries, (size_t)map_size) < 0) {
      free(entries);
      return -1;
    }

    if (crc32c(0, entries, (size_t)map_size) != footer.crc32) {
      fprintf(stderr, "btrfs2ext4: migration map CRC mismatch! Rollback "
                      "aborted to prevent corruption.\n");
      free(entries);
      return -1;
    }

    for (uint32_t i = 0; i < footer.entry_count; i++) {
      if (migration_map_validate_entry_bounds(dev, &entries[i]) < 0) {
        free(entries);
        return -1;
      }
    }

    printf("Reversing block relocations...\n");
    uint8_t *buf = malloc(1024 * 1024);
    if (!buf) {
      free(entries);
      return -1;
    }

    for (int32_t i = footer.entry_count - 1; i >= 0; i--) {
      struct relocation_entry *re = &entries[i];
      uint64_t remaining = re->length;
      uint64_t src = re->dst_offset;
      uint64_t dst = re->src_offset;
      while (remaining > 0) {
        uint32_t chunk = remaining > 1024 * 1024 ? 1024 * 1024 : remaining;
        if (device_read(dev, src, buf, chunk) < 0 ||
            device_write(dev, dst, buf, chunk) < 0) {
          free(buf);
          free(entries);
          return -1;
        }
        src += chunk;
        dst += chunk;
        remaining -= chunk;
      }
    }
    free(buf);
    free(entries);
    printf("Block relocations reversed.\n");
  }

  struct btrfs_super_block sb_backup;
  if (device_read(dev, backup_offset, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;
  if (device_write(dev, BTRFS_SUPER_OFFSET, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  memset(&footer, 0, sizeof(footer));
  if (device_write(dev, footer_offset, &footer, sizeof(footer)) < 0)
    return -1;
  if (device_sync(dev) < 0)
    return -1;
  return 0;
}
