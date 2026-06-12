#ifndef CONVERSION_ETA_H
#define CONVERSION_ETA_H

#include <stdint.h>

struct device;
struct ext4_layout;
struct ext4_space_budget;
struct relocation_plan;
struct btrfs_fs_info;

struct conversion_eta {
  double pass1_sec, pass2_reloc_sec, pass3_write_sec;
  double total_min_sec, total_max_sec;
  uint64_t pass2_read_bytes, pass2_write_bytes;
  uint64_t pass3_write_bytes;
  uint8_t device_rotational; /* 0=SSD/NVMe, 1=HDD, 2=unknown */
};

int conversion_eta_estimate(const struct device *dev,
                            const struct ext4_layout *layout,
                            const struct ext4_space_budget *budget,
                            const struct relocation_plan *reloc,
                            const struct btrfs_fs_info *fs,
                            double throughput_scale, struct conversion_eta *out);

#endif
