#include "conversion_eta.h"

#include "btrfs/btrfs_reader.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "relocator.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define HDD_READ_MBPS 120.0
#define HDD_WRITE_MBPS 80.0
#define SSD_READ_MBPS 400.0
#define SSD_WRITE_MBPS 300.0
#define ZERO_BW_MBPS 200.0
#define HDD_SEEK_MS 8.0
#define SSD_SEEK_MS 0.1

static uint8_t cached_rotational = 2;
static char cached_path[4096];

static int sysfs_rotational_for_name(const char *disk_name)
{
  char path[256];
  FILE *f;

  snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", disk_name);
  f = fopen(path, "r");
  if (!f)
    return -1;
  int val = -1;
  if (fscanf(f, "%d", &val) != 1)
    val = -1;
  fclose(f);
  return val;
}

static uint8_t detect_device_rotational(const char *device_path)
{
  const char *base;
  char disk[128];
  ssize_t len;
  char link[512];
  int rotational;

  if (!device_path || strncmp(device_path, "/dev/", 5) != 0)
    return 2;

  base = strrchr(device_path, '/');
  if (!base)
    return 2;
  base++;

  snprintf(disk, sizeof(disk), "%s", base);
  while (strlen(disk) > 0 && disk[strlen(disk) - 1] >= '0' &&
         disk[strlen(disk) - 1] <= '9')
    disk[strlen(disk) - 1] = '\0';
  if (disk[0] == '\0')
    return 2;

  rotational = sysfs_rotational_for_name(disk);
  if (rotational >= 0)
    return (uint8_t)rotational;

  snprintf(link, sizeof(link), "/sys/class/block/%s", base);
  len = readlink(link, link, sizeof(link) - 1);
  if (len > 0) {
    link[len] = '\0';
    const char *slash = strrchr(link, '/');
    if (slash) {
      rotational = sysfs_rotational_for_name(slash + 1);
      if (rotational >= 0)
        return (uint8_t)rotational;
    }
  }

  return 2;
}

static uint8_t device_rotational_cached(const struct device *dev)
{
  if (!dev)
    return 2;
  if (cached_path[0] && strcmp(cached_path, dev->path) == 0)
    return cached_rotational;
  cached_rotational = detect_device_rotational(dev->path);
  strncpy(cached_path, dev->path, sizeof(cached_path) - 1);
  cached_path[sizeof(cached_path) - 1] = '\0';
  return cached_rotational;
}

static double mbps_to_bytes(double mbps) { return mbps * 1024.0 * 1024.0; }

int conversion_eta_estimate(const struct device *dev,
                            const struct ext4_layout *layout,
                            const struct ext4_space_budget *budget,
                            const struct relocation_plan *reloc,
                            const struct btrfs_fs_info *fs,
                            double throughput_scale, struct conversion_eta *out)
{
  double read_bw, write_bw, seek_ms, spread;
  uint64_t reloc_bytes = 0;
  uint64_t meta_bytes;
  uint64_t journal_zero_bytes;

  if (!layout || !budget || !reloc || !out)
    return -1;

  memset(out, 0, sizeof(*out));
  if (throughput_scale <= 0.0)
    throughput_scale = 1.0;

  out->device_rotational = device_rotational_cached(dev);
  if (out->device_rotational == 1) {
    read_bw = mbps_to_bytes(HDD_READ_MBPS * throughput_scale);
    write_bw = mbps_to_bytes(HDD_WRITE_MBPS * throughput_scale);
    seek_ms = HDD_SEEK_MS;
    spread = 0.20;
  } else {
    read_bw = mbps_to_bytes(SSD_READ_MBPS * throughput_scale);
    write_bw = mbps_to_bytes(SSD_WRITE_MBPS * throughput_scale);
    seek_ms = SSD_SEEK_MS;
    spread = 0.10;
  }

  if (fs) {
    out->pass1_sec = (double)fs->inode_count * 4096.0 / read_bw;
    if (out->pass1_sec < 1.0)
      out->pass1_sec = 1.0;
  } else {
    out->pass1_sec = 1.0;
  }

  for (uint32_t i = 0; i < reloc->count; i++)
    reloc_bytes += reloc->entries[i].length;
  out->pass2_read_bytes = reloc_bytes;
  out->pass2_write_bytes = reloc_bytes;
  if (reloc_bytes > 0 && read_bw > 0.0)
    out->pass2_reloc_sec =
        ((double)reloc_bytes * 2.0 / read_bw) +
        ((double)reloc->count * seek_ms / 1000.0);

  meta_bytes = (uint64_t)layout->total_inodes * layout->inode_size;
  meta_bytes += (uint64_t)layout->num_groups * layout->desc_size;
  meta_bytes += (uint64_t)layout->num_groups * layout->block_size * 2;
  out->pass3_write_bytes =
      (uint64_t)budget->total_required * layout->block_size + meta_bytes;

  journal_zero_bytes = (uint64_t)budget->journal_blocks * layout->block_size;
  if (write_bw > 0.0)
    out->pass3_write_sec = (double)out->pass3_write_bytes / write_bw;
  if (journal_zero_bytes > 0)
    out->pass3_write_sec +=
        (double)journal_zero_bytes / mbps_to_bytes(ZERO_BW_MBPS);

  {
    double total =
        out->pass1_sec + out->pass2_reloc_sec + out->pass3_write_sec;
    out->total_min_sec = total * (1.0 - spread);
    out->total_max_sec = total * (1.0 + spread);
    if (out->total_min_sec < 0.0)
      out->total_min_sec = 0.0;
  }

  return 0;
}
