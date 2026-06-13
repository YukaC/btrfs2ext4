/*
 * mem_tracker.c — Unified memory policy
 *
 * Combines allocation tracking (75% MemAvailable guard) with adaptive
 * mmap/workdir configuration formerly in adaptive_mem_config.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "mem_tracker.h"

static uint64_t g_mem_used = 0;
static uint64_t g_track_threshold = 0;
static uint64_t g_mem_available = 0;
static int g_initialized = 0;
static struct mem_config g_mem_cfg;
static const struct mem_config *g_mem_cfg_ptr = NULL;

static void mem_track_init_thresholds(void) {
  g_mem_used = 0;
  g_initialized = 1;

  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) {
    g_mem_available = (uint64_t)16 * 1024 * 1024 * 1024;
    g_track_threshold = g_mem_available;
    return;
  }

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "MemAvailable:", 13) == 0) {
      uint64_t kb = 0;
      if (sscanf(line + 13, " %lu", (unsigned long *)&kb) == 1)
        g_mem_available = kb * 1024;
      break;
    }
  }
  fclose(f);

  if (g_mem_available == 0)
    g_mem_available = (uint64_t)8 * 1024 * 1024 * 1024;

  g_track_threshold = (g_mem_available * 3) / 4;
}

void mem_config_init(struct mem_config *cfg, uint32_t memory_limit_mb,
                     const char *workdir) {
  memset(cfg, 0, sizeof(*cfg));

  long pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0)
    cfg->total_ram = (uint64_t)pages * (uint64_t)page_size;
  else
    cfg->total_ram = 2ULL * 1024 * 1024 * 1024;

  long avail_pages = sysconf(_SC_AVPHYS_PAGES);
  if (avail_pages > 0 && page_size > 0)
    cfg->available_ram = (uint64_t)avail_pages * (uint64_t)page_size;
  else
    cfg->available_ram = cfg->total_ram / 2;

  if (memory_limit_mb > 0)
    cfg->mmap_threshold = (uint64_t)memory_limit_mb * 1024 * 1024;
  else
    cfg->mmap_threshold = cfg->total_ram * 60 / 100;

  cfg->workdir = workdir ? workdir : ".";

  struct statfs sfs;
  if (statfs(cfg->workdir, &sfs) == 0 && sfs.f_type == 0x01021994)
    cfg->workdir_is_tmpfs = 1;

  mem_track_init_thresholds();
  cfg->track_threshold = g_track_threshold;

  g_mem_cfg = *cfg;
  g_mem_cfg_ptr = &g_mem_cfg;

  printf("[INFO] RAM detected:     %.1f GiB total, %.1f GiB available\n",
         (double)cfg->total_ram / (1024.0 * 1024.0 * 1024.0),
         (double)cfg->available_ram / (1024.0 * 1024.0 * 1024.0));
  printf("[INFO] mmap threshold:   %.0f MiB%s\n",
         (double)cfg->mmap_threshold / (1024.0 * 1024.0),
         memory_limit_mb > 0 ? " (user-configured)" : " (auto: 60%%)");
  printf("[INFO] Temp file dir:    %s%s\n\n", cfg->workdir,
         cfg->workdir_is_tmpfs ? " [tmpfs WARNING]" : "");

  if (cfg->workdir_is_tmpfs) {
    fprintf(stderr,
            "\n[WARNING] --workdir '%s' is mounted on tmpfs (RAM-backed).\n"
            "  Creating temp swap files here defeats the purpose of mmap!\n"
            "  Use a physical disk path instead.\n\n",
            cfg->workdir);
  }
}

const struct mem_config *mem_config_get(void) { return g_mem_cfg_ptr; }

void mem_track_alloc(size_t bytes) {
  if (!g_initialized)
    mem_track_init_thresholds();
  g_mem_used += bytes;
}

void mem_track_free(size_t bytes) {
  if (bytes <= g_mem_used)
    g_mem_used -= bytes;
  else
    g_mem_used = 0;
}

uint64_t mem_track_usage(void) { return g_mem_used; }

int mem_track_exceeded(void) {
  if (!g_initialized)
    mem_track_init_thresholds();
  return g_mem_used > g_track_threshold;
}

void mem_track_report(void) {
  if (!g_initialized)
    return;

  printf("  Memory usage:     %.1f MiB / %.1f MiB available (%.0f%% of "
         "threshold)\n",
         (double)g_mem_used / (1024.0 * 1024.0),
         (double)g_mem_available / (1024.0 * 1024.0),
         g_track_threshold > 0 ? (double)g_mem_used * 100.0 / g_track_threshold
                               : 0.0);

  if (g_mem_used > g_track_threshold) {
    fprintf(stderr,
            "  WARNING: memory usage exceeds 75%% of available RAM!\n"
            "  Disabling optional hash tables to reduce memory pressure.\n");
  }
}
