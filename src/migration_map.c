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

int migration_map_save(struct device *dev, const struct relocation_plan *plan) {
  /* Write btrfs superblock backup */
  struct btrfs_super_block sb_backup;
  if (device_read(dev, BTRFS_SUPER_OFFSET, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  uint64_t backup_offset = (dev->size - SUPERBLOCK_BACKUP_OFFSET) & ~4095ULL;
  if (device_write(dev, backup_offset, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  /* Calculate map size and offset */
  if (plan->count == 0)
    return 0;

  /* Hard upper bound to avoid pathological plans consuming absurd space */
  const uint32_t MIGRATION_MAX_ENTRIES = 1024 * 1024; /* ~40–64 MiB of map */
  if (plan->count > MIGRATION_MAX_ENTRIES) {
    fprintf(stderr,
            "btrfs2ext4: migration plan has %u entries, exceeds max %u — "
            "refusing to save migration map\n",
            plan->count, MIGRATION_MAX_ENTRIES);
    return -1;
  }

  uint64_t map_size = (uint64_t)plan->count * sizeof(struct relocation_entry);

  /* Secondary safety cap: never try to reserve more than 1 GiB for the map */
  if (map_size > 1024ULL * 1024 * 1024) {
    fprintf(stderr,
            "btrfs2ext4: migration map too large (%lu bytes) — corrupted plan "
            "or unsupported scale\n",
            (unsigned long)map_size);
    return -1;
  }

  uint64_t map_offset = backup_offset - MIGRATION_FOOTER_OFFSET - map_size;
  map_offset &= ~4095ULL; /* block aligned */

  /* Write the entries array */
  if (plan->count > 0) {
    if (device_write(dev, map_offset, plan->entries, map_size) < 0)
      return -1;
  }

  /* Create and write footer */
  struct migration_footer footer;
  memset(&footer, 0, sizeof(footer));
  memcpy(footer.magic, MIGRATION_MAGIC, 8);
  footer.map_offset = map_offset;
  footer.entry_count = plan->count;
  footer.crc32 = crc32c(0, plan->entries, map_size);

  uint64_t footer_offset = backup_offset - MIGRATION_FOOTER_OFFSET;
  if (device_write(dev, footer_offset, &footer, sizeof(footer)) < 0)
    return -1;

  device_sync(dev);
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

  printf("Found valid migration map with %u entries.\n", footer.entry_count);

  /* Read the relocation entries */
  if (footer.entry_count > 0) {
    uint64_t map_size =
        (uint64_t)footer.entry_count * sizeof(struct relocation_entry);

    /* Safely cap allocation limit to prevent extreme OOM attacks from bad
     * footers */
    if (map_size > 1024ULL * 1024 * 1024) { /* Max 1 GB map */
      fprintf(stderr,
              "btrfs2ext4: rollback migration map too large (corrupted?)\n");
      return -1;
    }

    struct relocation_entry *entries = malloc((size_t)map_size);
    if (!entries) {
      fprintf(stderr, "btrfs2ext4: rollback out of memory\n");
      return -1;
    }

    if (device_read(dev, footer.map_offset, entries, (size_t)map_size) < 0) {
      free(entries);
      return -1;
    }

    /* Verify CRC */
    uint32_t calc_crc = crc32c(0, entries, (size_t)map_size);
    if (calc_crc != footer.crc32) {
      fprintf(stderr, "btrfs2ext4: migration map CRC mismatch! Rollback "
                      "aborted to prevent corruption.\n");
      free(entries);
      return -1;
    }

    printf("Reversing block relocations...\n");

    /* Buffer for copying data */
    uint8_t *buf = malloc(1024 * 1024); /* 1MB copy buffer */
    if (!buf) {
      free(entries);
      return -1;
    }

    /* Iterate backwards. */
    for (int32_t i = footer.entry_count - 1; i >= 0; i--) {
      struct relocation_entry *re = &entries[i];

      uint64_t remaining = re->length;
      uint64_t src = re->dst_offset; /* reading from where we copied TO */
      uint64_t dst = re->src_offset; /* writing back to where we copied FROM */

      while (remaining > 0) {
        uint32_t chunk = remaining > 1024 * 1024 ? 1024 * 1024 : remaining;
        if (device_read(dev, src, buf, chunk) < 0) {
          fprintf(stderr,
                  "btrfs2ext4: rollback failed to read block at 0x%lx\n",
                  (unsigned long)src);
          free(buf);
          free(entries);
          return -1;
        }
        if (device_write(dev, dst, buf, chunk) < 0) {
          fprintf(stderr,
                  "btrfs2ext4: rollback failed to restore block at 0x%lx\n",
                  (unsigned long)dst);
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

  /* Restore btrfs superblock */
  struct btrfs_super_block sb_backup;
  if (device_read(dev, backup_offset, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  if (device_write(dev, BTRFS_SUPER_OFFSET, &sb_backup, sizeof(sb_backup)) <
      0) {
    fprintf(stderr, "btrfs2ext4: rollback failed to write btrfs superblock "
                    "back to 0x10000\n");
    return -1;
  }

  /* Wipe footer to prevent double-rollback */
  memset(&footer, 0, sizeof(footer));
  if (device_write(dev, footer_offset, &footer, sizeof(footer)) < 0)
    return -1;

  device_sync(dev);
  return 0;
}
