/*
 * journal.h — Crash-recovery journal for block relocation
 */

#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>

struct device;
struct relocation_entry;

#define JOURNAL_MAGIC 0x42324534 /* "B2E4" */

/* Journal header: written at a fixed location on the device */
struct journal_header {
  uint32_t magic;
  uint32_t version;
  uint32_t entry_count;
  uint32_t state;          /* 0=clean, 1=in-progress, 2=needs-rollback */
  uint64_t journal_offset; /* where journal entries start */
  uint32_t checksum;       /* CRC32 of this header */
} __attribute__((packed));

#define JOURNAL_STATE_CLEAN 0
#define JOURNAL_STATE_IN_PROGRESS 1
#define JOURNAL_STATE_ROLLBACK 2

/*
 * Initialize the crash-recovery journal.
 * Allocates space at the specified offset on the device.
 * Returns 0 on success, -1 on error.
 */
int journal_init(struct device *dev, uint64_t journal_offset);

/*
 * Log a relocation operation before executing it.
 * Returns 0 on success, -1 on error.
 */
int journal_log_move(struct device *dev, const struct relocation_entry *entry);

/*
 * Mark a relocation operation as completed.
 * Returns 0 on success, -1 on error.
 */
int journal_mark_complete(struct device *dev, uint32_t seq);

/*
 * Check for an incomplete journal on startup.
 * If found, returns the number of incomplete entries (> 0).
 * If clean, returns 0.
 * On error, returns -1.
 */
int journal_check(struct device *dev, uint64_t journal_offset);

/*
 * Replay/rollback an incomplete journal.
 * Reverses any partially-completed relocation operations.
 * Returns 0 on success, -1 on error.
 */
int journal_replay(struct device *dev, uint64_t journal_offset);

/*
 * Replay/rollback an incomplete journal partially up to a specific sequence.
 * Reverses any partially-completed relocation operations up to failed_seq.
 * Returns 0 on success, -1 on error.
 */
int journal_replay_partial(struct device *dev, uint64_t journal_offset,
                           uint32_t failed_seq);

/*
 * Returns the currently active journal offset.
 */
uint64_t journal_current_offset(void);

/*
 * Clear the journal (mark as clean).
 * Called after successful conversion.
 * Returns 0 on success, -1 on error.
 */
int journal_clear(struct device *dev, uint64_t journal_offset);

#endif /* JOURNAL_H */
