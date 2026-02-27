/*
 * journal.c — Crash-recovery journal (stub for v1)
 *
 * A minimal journaling implementation for the block relocator.
 * In v1, this provides basic write-ahead logging for relocation ops.
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_io.h"
#include "journal.h"
#include "relocator.h"

/* CRC32C from superblock.c */
extern uint32_t crc32c(uint32_t crc, const void *buf, size_t len);

static uint64_t g_journal_offset = 0;
static uint32_t g_journal_entries = 0;

uint64_t journal_current_offset(void) { return g_journal_offset; }

int journal_init(struct device *dev, uint64_t journal_offset) {
  g_journal_offset = journal_offset;
  g_journal_entries = 0;

  struct journal_header hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = htole32(JOURNAL_MAGIC);
  hdr.version = htole32(1);
  hdr.entry_count = htole32(0);
  hdr.state = htole32(JOURNAL_STATE_IN_PROGRESS);
  hdr.journal_offset = htole64(journal_offset);

  /* Compute checksum */
  hdr.checksum = 0;
  hdr.checksum = htole32(crc32c(0, &hdr, sizeof(hdr)));

  if (device_write(dev, journal_offset, &hdr, sizeof(hdr)) < 0)
    return -1;

  return device_sync(dev);
}

int journal_log_move(struct device *dev, const struct relocation_entry *entry) {
  /* Write relocation entry after the header + existing entries */
  uint64_t entry_offset = g_journal_offset + sizeof(struct journal_header) +
                          g_journal_entries * sizeof(struct relocation_entry);

  if (device_write(dev, entry_offset, entry, sizeof(*entry)) < 0)
    return -1;

  g_journal_entries++;

  /* Update header entry count */
  uint32_t count_le = htole32(g_journal_entries);
  if (device_write(
          dev, g_journal_offset + offsetof(struct journal_header, entry_count),
          &count_le, sizeof(count_le)) < 0)
    return -1;

  return device_sync(dev);
}

int journal_mark_complete(struct device *dev, uint32_t seq) {
  /* Find the entry with this sequence number and mark completed */
  uint64_t entry_offset = g_journal_offset + sizeof(struct journal_header) +
                          seq * sizeof(struct relocation_entry);

  uint8_t completed = 1;
  if (device_write(dev,
                   entry_offset + offsetof(struct relocation_entry, completed),
                   &completed, sizeof(completed)) < 0)
    return -1;

  return 0;
}

int journal_check(struct device *dev, uint64_t journal_offset) {
  struct journal_header hdr;
  if (device_read(dev, journal_offset, &hdr, sizeof(hdr)) < 0)
    return -1;

  if (le32toh(hdr.magic) != JOURNAL_MAGIC)
    return 0; /* No journal found, clean */

  /* Verify header checksum to detect partial/corrupt journals. */
  uint32_t stored_csum = le32toh(hdr.checksum);
  hdr.checksum = 0;
  uint32_t computed_csum = crc32c(0, &hdr, sizeof(hdr));
  if (stored_csum != computed_csum) {
    fprintf(stderr,
            "btrfs2ext4: journal header checksum mismatch "
            "(stored=0x%08x computed=0x%08x) — ignoring journal\n",
            stored_csum, computed_csum);
    return 0;
  }

  uint32_t state = le32toh(hdr.state);
  if (state == JOURNAL_STATE_CLEAN)
    return 0;

  if (state == JOURNAL_STATE_IN_PROGRESS) {
    /* Count incomplete entries */
    uint32_t count = le32toh(hdr.entry_count);
    int incomplete = 0;

    for (uint32_t i = 0; i < count; i++) {
      struct relocation_entry entry;
      uint64_t entry_offset = journal_offset + sizeof(struct journal_header) +
                              i * sizeof(struct relocation_entry);
      if (device_read(dev, entry_offset, &entry, sizeof(entry)) < 0)
        return -1;
      if (!entry.completed)
        incomplete++;
    }

    return incomplete;
  }

  return 0;
}

int journal_replay(struct device *dev, uint64_t journal_offset) {
  struct journal_header hdr;
  if (device_read(dev, journal_offset, &hdr, sizeof(hdr)) < 0)
    return -1;

  uint32_t count = le32toh(hdr.entry_count);
  printf("Replaying journal (%u entries)...\n", count);

  /* For each incomplete entry, reverse the move */
  for (int i = (int)count - 1; i >= 0; i--) {
    struct relocation_entry entry;
    uint64_t entry_offset = journal_offset + sizeof(struct journal_header) +
                            (uint32_t)i * sizeof(struct relocation_entry);
    if (device_read(dev, entry_offset, &entry, sizeof(entry)) < 0)
      return -1;

    if (entry.completed) {
      /* Completed move: reverse it (move back from dst to src) */
      uint64_t len = entry.length;
      if (len == 0) {
        continue;
      }

      /* Limit each replay chunk to 16 MiB para evitar OOM en footers corruptos.
       */
      const uint64_t MAX_JOURNAL_CHUNK = 16ULL * 1024 * 1024;
      if (len > MAX_JOURNAL_CHUNK)
        len = MAX_JOURNAL_CHUNK;

      /* Validar rangos dentro del dispositivo. */
      if (entry.dst_offset > dev->size || entry.src_offset > dev->size ||
          len > dev->size || entry.dst_offset > dev->size - len ||
          entry.src_offset > dev->size - len) {
        fprintf(stderr,
                "btrfs2ext4: journal replay entry %d has invalid offsets or "
                "length (src=0x%lx dst=0x%lx len=%lu)\n",
                i, (unsigned long)entry.src_offset,
                (unsigned long)entry.dst_offset, (unsigned long)entry.length);
        return -1;
      }

      uint8_t *buf = malloc((size_t)len);
      if (!buf)
        return -1;

      if (device_read(dev, entry.dst_offset, buf, (size_t)len) == 0) {
        device_write(dev, entry.src_offset, buf, (size_t)len);
      }
      free(buf);
    }
  }

  return journal_clear(dev, journal_offset);
}

int journal_replay_partial(struct device *dev, uint64_t journal_offset,
                           uint32_t limit_seq) {
  struct journal_header hdr;
  if (device_read(dev, journal_offset, &hdr, sizeof(hdr)) < 0)
    return -1;

  uint32_t count = le32toh(hdr.entry_count);
  printf("Replaying partial journal (up to %u entries)...\n", limit_seq);

  uint32_t start_idx = limit_seq < count ? limit_seq : count - 1;

  /* For each completed entry up to failed_seq, reverse the move */
  for (int i = (int)start_idx; i >= 0; i--) {
    struct relocation_entry entry;
    uint64_t entry_offset = journal_offset + sizeof(struct journal_header) +
                            (uint32_t)i * sizeof(struct relocation_entry);
    if (device_read(dev, entry_offset, &entry, sizeof(entry)) < 0)
      continue;

    if (entry.completed) {
      /* Completed move: reverse it (move back from dst to src) */
      uint64_t len = entry.length;
      if (len == 0)
        continue;

      const uint64_t MAX_JOURNAL_CHUNK = 16ULL * 1024 * 1024;
      if (len > MAX_JOURNAL_CHUNK)
        len = MAX_JOURNAL_CHUNK;

      if (entry.dst_offset > dev->size || entry.src_offset > dev->size ||
          len > dev->size || entry.dst_offset > dev->size - len ||
          entry.src_offset > dev->size - len) {
        fprintf(stderr,
                "btrfs2ext4: journal replay entry %d has invalid offsets\n", i);
        continue;
      }

      uint8_t *buf = malloc((size_t)len);
      if (!buf)
        continue;

      if (device_read(dev, entry.dst_offset, buf, (size_t)len) == 0) {
        device_write(dev, entry.src_offset, buf, (size_t)len);
      }
      free(buf);
    }
  }

  return journal_clear(dev, journal_offset);
}

int journal_clear(struct device *dev, uint64_t journal_offset) {
  struct journal_header hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = htole32(JOURNAL_MAGIC);
  hdr.version = htole32(1);
  hdr.state = htole32(JOURNAL_STATE_CLEAN);

  if (device_write(dev, journal_offset, &hdr, sizeof(hdr)) < 0)
    return -1;

  return device_sync(dev);
}
