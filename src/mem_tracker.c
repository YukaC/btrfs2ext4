/*
 * mem_tracker.c — Memory usage tracker
 *
 * Reads MemAvailable from /proc/meminfo at init time and tracks the
 * cumulative allocations made by the converter. If the tracked usage
 * exceeds 75% of available memory, callers can disable optional data
 * structures (e.g., hash tables) and fall back to linear scans.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem_tracker.h"

static uint64_t g_mem_used = 0;
static uint64_t g_mem_threshold = 0;
static uint64_t g_mem_available = 0;
static int g_initialized = 0;

void mem_track_init(void) {
  g_mem_used = 0;
  g_mem_threshold = 0;
  g_mem_available = 0;
  g_initialized = 1;

  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) {
    /* Non-Linux or /proc not available — set very high threshold */
    g_mem_available = (uint64_t)16 * 1024 * 1024 * 1024; /* 16 GiB fallback */
    g_mem_threshold = g_mem_available;
    return;
  }

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "MemAvailable:", 13) == 0) {
      uint64_t kb = 0;
      if (sscanf(line + 13, " %lu", (unsigned long *)&kb) == 1) {
        g_mem_available = kb * 1024; /* Convert to bytes */
      }
      break;
    }
  }
  fclose(f);

  if (g_mem_available == 0) {
    /* Fallback if MemAvailable not found (very old kernels) */
    g_mem_available = (uint64_t)8 * 1024 * 1024 * 1024; /* 8 GiB */
  }

  /* Set threshold to 75% of available memory */
  g_mem_threshold = (g_mem_available * 3) / 4;
}

void mem_track_alloc(size_t bytes) {
  if (!g_initialized)
    mem_track_init();
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
    mem_track_init();
  return g_mem_used > g_mem_threshold;
}

void mem_track_report(void) {
  if (!g_initialized)
    return;

  printf("  Memory usage:     %.1f MiB / %.1f MiB available (%.0f%% of "
         "threshold)\n",
         (double)g_mem_used / (1024.0 * 1024.0),
         (double)g_mem_available / (1024.0 * 1024.0),
         g_mem_threshold > 0 ? (double)g_mem_used * 100.0 / g_mem_threshold
                             : 0.0);

  if (g_mem_used > g_mem_threshold) {
    fprintf(stderr,
            "  WARNING: memory usage exceeds 75%% of available RAM!\n"
            "  Disabling optional hash tables to reduce memory pressure.\n");
  }
}
