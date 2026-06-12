/*
 * emergency_recover.c — Pass 3 diagnostic (Approach B, read-only)
 */
#include <endian.h>
#include <stdio.h>
#include <string.h>
#include "btrfs/btrfs_structures.h"
#include "btrfs2ext4.h"
#include "device_io.h"
#include "ext4/ext4_structures.h"
#include "migration_map.h"

typedef enum { ER_CLEAN, ER_PASS2, ER_HYBRID, ER_DONE } er_state_t;

static int probe_ext4(struct device *d, int *p) {
  struct ext4_super_block sb;
  *p = 0;
  if (device_read(d, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) < 0) return -1;
  *p = le16toh(sb.s_magic) == EXT4_SUPER_MAGIC;
  return 0;
}
static int probe_btrfs(struct device *d, int *p) {
  struct btrfs_super_block sb;
  *p = 0;
  if (device_read(d, BTRFS_SUPER_OFFSET, &sb, sizeof(sb)) < 0) return -1;
  *p = le64toh(sb.magic) == BTRFS_MAGIC;
  return 0;
}
static er_state_t classify(int map, int ext4) {
  if (!map && !ext4) return ER_CLEAN;
  if (map && !ext4) return ER_PASS2;
  if (map && ext4) return ER_HYBRID;
  return ER_DONE;
}
static const char *label(er_state_t s) {
  switch (s) {
  case ER_CLEAN: return "clean";
  case ER_PASS2: return "pass2-only";
  case ER_HYBRID: return "hybrid";
  case ER_DONE: return "completed-ext4";
  }
  return "?";
}

int btrfs2ext4_emergency_recover(const char *device_path) {
  struct device dev;
  int map, ext4, btrfs;
  printf("Emergency recovery diagnostic for %s\n", device_path);
  printf("(read-only inspection — no changes will be made)\n\n");
  if (device_open(&dev, device_path, 1) < 0) return -1;
  int mr = migration_map_footer_present(&dev);
  if (mr < 0) { fprintf(stderr, "btrfs2ext4: failed to read migration map footer\n"); device_close(&dev); return -1; }
  map = mr;
  if (probe_ext4(&dev, &ext4) < 0 || probe_btrfs(&dev, &btrfs) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to probe superblock markers\n"); device_close(&dev); return -1;
  }
  er_state_t st = classify(map, ext4);
  printf("Markers:\n  migration map (%s): %s\n  ext4 magic (0x%04X @ 0x%04X): %s\n  btrfs magic @ 0x%lX: %s\n\nClassification: %s\n\n",
         MIGRATION_MAGIC, map ? "present" : "absent", EXT4_SUPER_MAGIC, EXT4_SUPER_OFFSET,
         ext4 ? "present" : "absent", (unsigned long)BTRFS_SUPER_OFFSET, btrfs ? "present" : "absent", label(st));
  switch (st) {
  case ER_CLEAN:
    printf("No interrupted btrfs2ext4 conversion detected on this device.\n"); break;
  case ER_PASS2:
    printf("Pass 2 checkpoint found; Pass 3 has not started.\nRecommended: btrfs2ext4 --rollback %s\nThen: btrfs check %s\n", device_path, device_path); break;
  case ER_HYBRID:
    printf("HYBRID STATE: migration map and Ext4 superblock both present.\nDO NOT run e2fsck or btrfs check blindly.\nRecommended:\n  1. btrfs2ext4 --rollback %s\n  2. btrfs check %s\n", device_path, device_path); break;
  case ER_DONE:
    printf("Ext4 superblock without migration map.\nRecommended: e2fsck -f %s\n", device_path); break;
  }
  device_close(&dev);
  return 0;
}
