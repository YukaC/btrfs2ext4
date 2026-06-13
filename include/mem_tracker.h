/*
 * mem_tracker.h — Unified memory policy for btrfs2ext4
 *
 * Single API for allocation tracking, RAM detection, mmap thresholds,
 * and optional-structure degradation (hash tables, bloom filters).
 */

#ifndef MEM_TRACKER_H
#define MEM_TRACKER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Unified memory configuration — replaces the former adaptive_mem_config
 * scattered across main.c and btrfs_reader.h.
 */
struct mem_config {
  uint64_t total_ram;       /* physical RAM (sysconf) */
  uint64_t available_ram;   /* currently available RAM */
  uint64_t track_threshold; /* mem_track_exceeded cutoff (75% available) */
  uint64_t mmap_threshold;  /* switch inode map to mmap (60% total or -m) */
  const char *workdir;      /* --workdir for temp mmap files */
  int workdir_is_tmpfs;     /* 1 = workdir on tmpfs (warn user) */
};

/*
 * Detect hardware, set thresholds, and initialize the global tracker.
 * memory_limit_mb: 0 = auto (60%% of total RAM for mmap threshold).
 */
void mem_config_init(struct mem_config *cfg, uint32_t memory_limit_mb,
                     const char *workdir);

/* Global singleton set by mem_config_init(); NULL until init. */
const struct mem_config *mem_config_get(void);

void mem_track_alloc(size_t bytes);
void mem_track_free(size_t bytes);
uint64_t mem_track_usage(void);
int mem_track_exceeded(void);
void mem_track_report(void);

#endif /* MEM_TRACKER_H */
