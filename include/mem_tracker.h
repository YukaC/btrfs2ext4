/*
 * mem_tracker.h — Memory usage tracker
 *
 * Lightweight memory usage monitoring to prevent OOM-killer situations
 * on systems converting large, fragmented filesystems.
 */

#ifndef MEM_TRACKER_H
#define MEM_TRACKER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Initialize the memory tracker.
 * Reads available system memory from /proc/meminfo and sets the threshold
 * to 75% of MemAvailable.
 */
void mem_track_init(void);

/*
 * Track a memory allocation of the given size.
 */
void mem_track_alloc(size_t bytes);

/*
 * Track a memory deallocation of the given size.
 */
void mem_track_free(size_t bytes);

/*
 * Returns the current tracked memory usage in bytes.
 */
uint64_t mem_track_usage(void);

/*
 * Returns 1 if current tracked usage exceeds the safety threshold, 0 otherwise.
 * When exceeded, callers should disable optional hash tables and fall back to
 * linear scan to reduce memory pressure.
 */
int mem_track_exceeded(void);

/*
 * Print memory usage summary.
 */
void mem_track_report(void);

#endif /* MEM_TRACKER_H */
