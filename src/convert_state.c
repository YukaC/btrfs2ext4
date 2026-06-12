/*
 * convert_state.c — Pass 3 conversion progress footer (Phase 3.5)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "btrfs2ext4.h"
#include "convert_state.h"
#include "device_io.h"
#include "migration_map.h"

extern uint32_t crc32c(uint32_t crc, const void *buf, size_t len);

static void tail_offsets(uint64_t dev_size, uint64_t *backup_offset,
                         uint64_t *footer_offset) {
  *backup_offset = (dev_size - SUPERBLOCK_BACKUP_OFFSET) & ~4095ULL;
  *footer_offset = *backup_offset - MIGRATION_FOOTER_OFFSET;
}

static uint32_t footer_crc(const struct convert_state_footer *footer) {
  struct convert_state_footer tmp = *footer;
  tmp.crc32 = 0;
  return crc32c(0, &tmp, sizeof(tmp));
}

int convert_state_compute_offset(struct device *dev, uint64_t *offset_out) {
  uint64_t backup_offset, footer_offset;
  tail_offsets(dev->size, &backup_offset, &footer_offset);

  struct migration_footer mf;
  if (device_read(dev, footer_offset, &mf, sizeof(mf)) < 0)
    return -1;

  uint64_t map_offset = footer_offset;
  if (memcmp(mf.magic, MIGRATION_MAGIC, 8) == 0) {
    uint64_t map_size = 0;
    if (migration_map_compute_layout(dev->size, mf.entry_count, &map_offset,
                                     &footer_offset, &backup_offset,
                                     &map_size) < 0)
      return -1;
  }

  if (map_offset < CONVERT_STATE_SIZE)
    return -1;
  *offset_out = map_offset - CONVERT_STATE_SIZE;
  return 0;
}

int convert_state_present(struct device *dev) {
  struct convert_state_footer footer;
  uint64_t offset;

  if (convert_state_compute_offset(dev, &offset) < 0)
    return -1;
  if (device_read(dev, offset, &footer, sizeof(footer)) < 0)
    return -1;
  return memcmp(footer.magic, CONVERT_STATE_MAGIC, 8) == 0 ? 1 : 0;
}

int convert_state_load(struct device *dev, struct convert_state_footer *out) {
  uint64_t offset;

  if (!out || convert_state_compute_offset(dev, &offset) < 0)
    return -1;
  if (device_read(dev, offset, out, sizeof(*out)) < 0)
    return -1;
  if (memcmp(out->magic, CONVERT_STATE_MAGIC, 8) != 0)
    return -1;
  if (footer_crc(out) != out->crc32) {
    fprintf(stderr, "btrfs2ext4: convert state CRC mismatch\n");
    return -1;
  }
  return 0;
}

int convert_state_save(struct device *dev, uint32_t phase,
                       uint32_t pass3_step_mask) {
  struct convert_state_footer footer;
  uint64_t offset;

  if (convert_state_compute_offset(dev, &offset) < 0)
    return -1;

  memset(&footer, 0, sizeof(footer));
  memcpy(footer.magic, CONVERT_STATE_MAGIC, 8);
  footer.phase = phase;
  footer.pass3_step_mask = pass3_step_mask;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  footer.timestamp = (uint64_t)ts.tv_sec;
  footer.crc32 = footer_crc(&footer);

  if (device_write(dev, offset, &footer, sizeof(footer)) < 0)
    return -1;
  return device_sync(dev) < 0 ? -1 : 0;
}

int convert_state_set_step(struct device *dev, uint32_t step_bit) {
  struct convert_state_footer footer;
  uint32_t phase = CONVERT_STATE_PHASE_PASS3;
  uint32_t mask = step_bit;

  if (convert_state_load(dev, &footer) == 0) {
    phase = footer.phase;
    mask = footer.pass3_step_mask | step_bit;
    if (phase < CONVERT_STATE_PHASE_PASS3)
      phase = CONVERT_STATE_PHASE_PASS3;
  }

  return convert_state_save(dev, phase, mask);
}

int convert_state_clear(struct device *dev) {
  struct convert_state_footer footer;
  uint64_t offset;

  if (convert_state_compute_offset(dev, &offset) < 0)
    return -1;
  memset(&footer, 0, sizeof(footer));
  if (device_write(dev, offset, &footer, sizeof(footer)) < 0)
    return -1;
  return device_sync(dev) < 0 ? -1 : 0;
}

static void print_pass3_steps(uint32_t mask) {
  printf("Pass 3 steps completed (bitmask 0x%02x):\n", mask);
  if (mask & CONVERT_STATE_STEP_SUPERBLOCK)
    printf("  [0x01] superblock written\n");
  if (mask & CONVERT_STATE_STEP_GDT)
    printf("  [0x02] GDT written\n");
  if (mask & CONVERT_STATE_STEP_INODES)
    printf("  [0x04] inode tables written\n");
  if (mask & CONVERT_STATE_STEP_DIRS)
    printf("  [0x08] directories written\n");
  if (mask & CONVERT_STATE_STEP_JOURNAL)
    printf("  [0x10] journal written\n");
  if (mask & CONVERT_STATE_STEP_BITMAPS)
    printf("  [0x20] bitmaps finalized\n");
  if (mask & CONVERT_STATE_STEP_FREE_COUNTS)
    printf("  [0x40] free counts updated\n");
  if (mask & CONVERT_STATE_STEP_COMPLETE)
    printf("  [0x80] conversion complete\n");
}

int convert_state_emergency_recover(struct device *dev) {
  int map_present = migration_map_footer_present(dev);
  struct convert_state_footer cs;
  int cs_valid = convert_state_load(dev, &cs);

  if (map_present <= 0 && cs_valid < 0) {
    fprintf(stderr,
            "btrfs2ext4: no interrupted conversion detected "
            "(no CONVERT_STATE or B2E4MAP1).\n");
    return -1;
  }

  if (cs_valid < 0) {
    if (map_present > 0) {
      fprintf(stderr,
              "btrfs2ext4: Pass 2 checkpoint found — run:\n"
              "  btrfs2ext4 --rollback %s\n",
              dev->path);
      return -1;
    }
    fprintf(stderr,
            "btrfs2ext4: no interrupted conversion detected "
            "(no CONVERT_STATE or B2E4MAP1).\n");
    return -1;
  }

  if (cs.pass3_step_mask & CONVERT_STATE_STEP_COMPLETE) {
    printf("Conversion already completed; run: e2fsck -f %s\n", dev->path);
    return 0;
  }

  uint32_t pass3_bits = cs.pass3_step_mask & 0x7F;
  if (pass3_bits == 0) {
    fprintf(stderr,
            "btrfs2ext4: Pass 2 checkpoint only — run:\n"
            "  btrfs2ext4 --rollback %s\n",
            dev->path);
    return -1;
  }

  printf("=== Emergency recovery (Pass 3 interrupted) ===\n\n");
  print_pass3_steps(cs.pass3_step_mask);
  fprintf(stderr,
          "\nWARNING: The device is in a hybrid state (partial Ext4 metadata "
          "with Btrfs data).\n"
          "Do NOT mount as Ext4 or run e2fsck blindly.\n\n");

  if (migration_map_rollback(dev) < 0) {
    fprintf(stderr, "btrfs2ext4: emergency rollback of relocations failed.\n");
    return -1;
  }

  convert_state_clear(dev);

  printf("\nEmergency recovery complete.\n");
  printf("Btrfs superblock restored from backup.\n");
  printf("\nRecommended next steps:\n");
  printf("  1. btrfs check %s   # verify Btrfs integrity\n", dev->path);
  printf("  2. btrfs scrub %s     # optional data verification\n", dev->path);
  printf("  3. If unsure, clone with ddrescue and retry on a copy\n");
  printf("  4. Restore from an external backup if available (safest)\n");

  return 0;
}

int btrfs2ext4_emergency_recover(const char *device_path) {
  struct device dev;

  printf("Emergency recovery for %s...\n", device_path);

  if (device_open(&dev, device_path, 0) < 0)
    return -1;

  int ret = convert_state_emergency_recover(&dev);
  device_close(&dev);
  return ret;
}
