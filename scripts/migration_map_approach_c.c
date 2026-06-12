/*
 * migration_map.c — Persistent Block Relocation Journal (Phase 3 Approach C)
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

static void tail_offsets(uint64_t dev_size, uint64_t *backup_offset,
                         uint64_t *footer_offset) {
  *backup_offset = (dev_size - SUPERBLOCK_BACKUP_OFFSET) & ~4095ULL;
  *footer_offset = *backup_offset - MIGRATION_FOOTER_OFFSET;
}

static int cmp_seq_desc(const void *a, const void *b) {
  const struct relocation_entry *ea = a;
  const struct relocation_entry *eb = b;
  if (ea->seq > eb->seq)
    return -1;
  if (ea->seq < eb->seq)
    return 1;
  return 0;
}

static int read_footer(struct device *dev, struct migration_footer *footer,
                       uint64_t *backup_offset, uint64_t *footer_offset) {
  tail_offsets(dev->size, backup_offset, footer_offset);
  return device_read(dev, *footer_offset, footer, sizeof(*footer));
}

static int write_footer(struct device *dev, uint64_t footer_offset,
                        uint64_t map_offset, uint32_t entry_count,
                        uint32_t crc, uint32_t checkpoint_interval) {
  struct migration_footer footer;
  memset(&footer, 0, sizeof(footer));
  memcpy(footer.magic, MIGRATION_MAGIC, 8);
  footer.map_offset = map_offset;
  footer.entry_count = entry_count;
  footer.crc32 = crc;
  footer.checkpoint_interval = checkpoint_interval;
  return device_write(dev, footer_offset, &footer, sizeof(footer));
}

static int verify_dst_checksum(struct device *dev,
                               const struct relocation_entry *re) {
  uint8_t *buf = malloc(1024 * 1024);
  if (!buf)
    return -1;
  uint32_t calc = 0;
  uint64_t off = re->dst_offset;
  uint64_t rem = re->length;
  while (rem > 0) {
    uint32_t chunk = rem > 1024 * 1024 ? 1024 * 1024 : (uint32_t)rem;
    if (device_read(dev, off, buf, chunk) < 0) {
      free(buf);
      return -1;
    }
    calc = crc32c(calc, buf, chunk);
    off += chunk;
    rem -= chunk;
  }
  free(buf);
  return calc == re->checksum ? 0 : -1;
}

static int migration_map_validate_footer_bounds(struct device *dev,
                                                const struct migration_footer *footer) {
  if (footer->entry_count > MIGRATION_MAX_ENTRIES)
    return -1;
  uint64_t backup_offset, footer_offset;
  tail_offsets(dev->size, &backup_offset, &footer_offset);
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

int migration_map_compute_layout(uint64_t dev_size, uint32_t entry_count,
                                 uint64_t *map_offset, uint64_t *footer_offset,
                                 uint64_t *backup_offset, uint64_t *map_size) {
  tail_offsets(dev_size, backup_offset, footer_offset);
  *map_size = (uint64_t)entry_count * sizeof(struct relocation_entry);
  if (entry_count == 0) {
    *map_offset = *footer_offset;
    return 0;
  }
  if (*footer_offset < *map_size)
    return -1;
  *map_offset = (*footer_offset - *map_size) & ~4095ULL;
  if (*map_offset + *map_size > *footer_offset ||
      *map_offset + *map_size > dev_size)
    return -1;
  return 0;
}

int migration_map_footer_present(struct device *dev) {
  struct migration_footer footer;
  uint64_t bo, fo;
  if (read_footer(dev, &footer, &bo, &fo) < 0)
    return -1;
  return memcmp(footer.magic, MIGRATION_MAGIC, 8) == 0 ? 1 : 0;
}

int migration_map_detect_existing(struct device *dev) {
  return migration_map_footer_present(dev);
}

uint32_t migration_map_completed_count(const struct relocation_plan *plan) {
  uint32_t n = 0;
  if (!plan || !plan->entries)
    return 0;
  for (uint32_t i = 0; i < plan->count; i++) {
    if (plan->entries[i].completed)
      n++;
  }
  return n;
}

int migration_map_validate_plan(const struct relocation_entry *entries,
                                uint32_t count, uint64_t dev_size) {
  for (uint32_t i = 0; i < count; i++) {
    const struct relocation_entry *re = &entries[i];
    if (re->src_offset == re->dst_offset)
      return -1;
    if (re->length == 0 || re->src_offset + re->length > dev_size ||
        re->dst_offset + re->length > dev_size)
      return -1;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint64_t a0 = entries[i].dst_offset, a1 = a0 + entries[i].length;
    for (uint32_t j = i + 1; j < count; j++) {
      uint64_t b0 = entries[j].dst_offset, b1 = b0 + entries[j].length;
      if (a0 < b1 && b0 < a1)
        return -1;
    }
  }
  return 0;
}

int migration_map_preflight_space(struct device *dev, uint32_t entry_count) {
  uint64_t mo, ms, bo, fo;
  if (entry_count > MIGRATION_MAX_ENTRIES)
    return -1;
  ms = (uint64_t)entry_count * sizeof(struct relocation_entry);
  if (ms > 1024ULL * 1024 * 1024)
    return -1;
  return migration_map_compute_layout(dev->size, entry_count, &mo, &fo, &bo, &ms);
}

int migration_map_load(struct device *dev, struct relocation_plan *plan) {
  struct migration_footer footer;
  uint64_t backup_offset, footer_offset;
  if (read_footer(dev, &footer, &backup_offset, &footer_offset) < 0)
    return -1;
  if (memcmp(footer.magic, MIGRATION_MAGIC, 8) != 0)
    return -1;
  if (footer.entry_count > MIGRATION_MAX_ENTRIES)
    return -1;

  uint64_t map_offset, map_size =
      (uint64_t)footer.entry_count * sizeof(struct relocation_entry);
  if (migration_map_compute_layout(dev->size, footer.entry_count, &map_offset,
                                   &footer_offset, &backup_offset,
                                   &map_size) < 0)
    return -1;
  if (footer.map_offset != map_offset)
    return -1;

  if (footer.entry_count == 0) {
    free(plan->entries);
    plan->entries = NULL;
    plan->count = 0;
    plan->capacity = 0;
    return 0;
  }

  struct relocation_entry *entries = malloc((size_t)map_size);
  if (!entries)
    return -1;
  if (device_read(dev, footer.map_offset, entries, (size_t)map_size) < 0) {
    free(entries);
    return -1;
  }
  if (crc32c(0, entries, (size_t)map_size) != footer.crc32) {
    free(entries);
    return -1;
  }
  for (uint32_t i = 0; i < footer.entry_count; i++) {
    if (migration_map_validate_entry_bounds(dev, &entries[i]) < 0) {
      free(entries);
      return -1;
    }
  }
  free(plan->entries);
  plan->entries = entries;
  plan->count = footer.entry_count;
  plan->capacity = footer.entry_count;
  return 0;
}

int migration_map_flush_progress(struct device *dev,
                               const struct relocation_plan *plan) {
  struct migration_footer footer;
  uint64_t backup_offset, footer_offset;
  if (read_footer(dev, &footer, &backup_offset, &footer_offset) < 0)
    return -1;
  if (memcmp(footer.magic, MIGRATION_MAGIC, 8) != 0)
    return -1;
  if (footer.entry_count != plan->count)
    return -1;

  uint64_t map_size =
      (uint64_t)plan->count * sizeof(struct relocation_entry);
  if (plan->count > 0) {
    if (device_write(dev, footer.map_offset, plan->entries, map_size) < 0)
      return -1;
  }

  uint32_t crc = plan->count > 0 ? crc32c(0, plan->entries, map_size) : 0;
  uint32_t interval = footer.checkpoint_interval;
  if (interval == 0)
    interval = MIGRATION_DEFAULT_CHECKPOINT_INTERVAL;

  if (write_footer(dev, footer_offset, footer.map_offset, plan->count, crc,
                   interval) < 0)
    return -1;

  return device_sync(dev) < 0 ? -1 : 0;
}

int migration_map_save(struct device *dev, const struct relocation_plan *plan) {
  struct btrfs_super_block sb_backup;
  if (device_read(dev, BTRFS_SUPER_OFFSET, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  uint64_t backup_offset, footer_offset, map_offset = 0, map_size = 0;
  tail_offsets(dev->size, &backup_offset, &footer_offset);

  if (device_write(dev, backup_offset, &sb_backup, sizeof(sb_backup)) < 0)
    return -1;

  if (plan->count > 0) {
    if (plan->count > MIGRATION_MAX_ENTRIES)
      return -1;
    map_size = (uint64_t)plan->count * sizeof(struct relocation_entry);
    if (map_size > 1024ULL * 1024 * 1024)
      return -1;
    if (migration_map_compute_layout(dev->size, plan->count, &map_offset,
                                       &footer_offset, &backup_offset,
                                       &map_size) < 0)
      return -1;
    if (device_write(dev, map_offset, plan->entries, map_size) < 0)
      return -1;
  }

  uint32_t crc = plan->count > 0 ? crc32c(0, plan->entries, map_size) : 0;
  if (write_footer(dev, footer_offset, map_offset, plan->count, crc,
                   MIGRATION_DEFAULT_CHECKPOINT_INTERVAL) < 0)
    return -1;

  return device_sync(dev) < 0 ? -1 : 0;
}

int migration_map_rollback(struct device *dev) {
  uint64_t backup_offset, footer_offset;
  tail_offsets(dev->size, &backup_offset, &footer_offset);

  struct migration_footer footer;
  if (device_read(dev, footer_offset, &footer, sizeof(footer)) < 0)
    return -1;

  if (memcmp(footer.magic, MIGRATION_MAGIC, 8) != 0) {
    int cleared = 1;
    for (int i = 0; i < 8; i++) {
      if (footer.magic[i] != 0) {
        cleared = 0;
        break;
      }
    }
    if (cleared) {
      printf("Migration map already cleared — nothing to roll back.\n");
      return 0;
    }
    fprintf(stderr, "btrfs2ext4: no valid migration map found for rollback\n");
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

    uint32_t n = 0;
    for (uint32_t i = 0; i < footer.entry_count; i++) {
      if (entries[i].completed)
        n++;
    }

    struct relocation_entry *todo =
        malloc((size_t)n * sizeof(struct relocation_entry));
    uint8_t *buf = malloc(1024 * 1024);
    if (!todo || !buf) {
      free(todo);
      free(buf);
      free(entries);
      return -1;
    }

    uint32_t w = 0;
    for (uint32_t i = 0; i < footer.entry_count; i++) {
      if (entries[i].completed)
        todo[w++] = entries[i];
    }
    qsort(todo, n, sizeof(struct relocation_entry), cmp_seq_desc);

    printf("Reversing %u completed relocations...\n", n);
    for (uint32_t i = 0; i < n; i++) {
      struct relocation_entry *re = &todo[i];
      if (verify_dst_checksum(dev, re) < 0) {
        free(todo);
        free(buf);
        free(entries);
        return -1;
      }
      uint64_t remaining = re->length;
      uint64_t src = re->dst_offset;
      uint64_t dst = re->src_offset;
      while (remaining > 0) {
        uint32_t chunk = remaining > 1024 * 1024 ? 1024 * 1024 : remaining;
        if (device_read(dev, src, buf, chunk) < 0 ||
            device_write(dev, dst, buf, chunk) < 0) {
          free(todo);
          free(buf);
          free(entries);
          return -1;
        }
        src += chunk;
        dst += chunk;
        remaining -= chunk;
      }
    }
    free(todo);
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

  return device_sync(dev) < 0 ? -1 : 0;
}
