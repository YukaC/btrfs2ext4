/*
 * device_io.h — Low-level device I/O abstraction
 *
 * Provides safe read/write/sync operations on block devices and image files.
 * All operations use absolute byte offsets. Writes are always followed by
 * fdatasync to ensure durability (critical for crash recovery).
 *
 * Optional io_uring batch API: when liburing is available, allows queueing
 * multiple writes and submitting them in a single syscall for maximum
 * throughput on NVMe/SSD devices.
 */

#ifndef DEVICE_IO_H
#define DEVICE_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef HAVE_IO_URING
#include <liburing.h>
#endif

/* Maximum number of in-flight I/O operations for batch API */
#define DEVICE_BATCH_QUEUE_DEPTH 256

/* Opaque device handle */
struct device {
  int fd;
  uint64_t size;   /* total device/file size in bytes */
  int read_only;   /* 1 = opened read-only (dry-run mode) */
  char path[4096]; /* device path for error messages */

#ifdef HAVE_IO_URING
  struct io_uring ring;   /* io_uring instance for batch I/O */
  int ring_initialized;   /* 1 if ring has been set up */
  uint32_t batch_pending; /* number of SQEs queued but not yet submitted */
#endif
};

/*
 * Open a device or image file.
 * If read_only is non-zero, opens O_RDONLY (for dry-run mode).
 * Returns 0 on success, -1 on error (errno set).
 */
int device_open(struct device *dev, const char *path, int read_only);

/*
 * Close the device.
 */
void device_close(struct device *dev);

/*
 * Read exactly 'size' bytes from device at 'offset' into 'buf'.
 * Returns 0 on success, -1 on error.
 */
int device_read(struct device *dev, uint64_t offset, void *buf, size_t size);

/*
 * Write exactly 'size' bytes from 'buf' to device at 'offset'.
 * Returns 0 on success, -1 on error.
 * Fails if device was opened read-only.
 */
int device_write(struct device *dev, uint64_t offset, const void *buf,
                 size_t size);

/*
 * Force sync all pending writes to disk.
 * Returns 0 on success, -1 on error.
 */
int device_sync(struct device *dev);

/*
 * Get device size in bytes.
 */
uint64_t device_get_size(struct device *dev);

/* ========================================================================
 * Batch Write API — io_uring accelerated (optional)
 *
 * Usage:
 *   device_write_batch_begin(dev);         // initialize ring
 *   for (...) {
 *     device_write_batch_add(dev, off, buf, len);  // queue SQE
 *     if (pending == QUEUE_DEPTH)
 *       device_write_batch_submit(dev);    // flush when full
 *   }
 *   device_write_batch_submit(dev);        // flush remaining
 *
 * When HAVE_IO_URING is not defined, _add() calls pwrite() immediately
 * and _begin()/_submit() are no-ops. Zero overhead, identical semantics.
 * ======================================================================== */

/*
 * Initialize the io_uring ring for batch writes.
 * Returns 0 on success, -1 on error.
 * No-op when io_uring is not available.
 */
int device_write_batch_begin(struct device *dev);

/*
 * Queue a write operation for batch submission.
 * The buffer must remain valid until device_write_batch_submit() returns.
 * Returns 0 on success, -1 on error.
 * When io_uring is not available, writes immediately via pwrite().
 */
int device_write_batch_add(struct device *dev, uint64_t offset, const void *buf,
                           size_t size);

/*
 * Submit all queued writes and wait for completion.
 * Returns 0 if all writes succeeded, -1 if any failed.
 * No-op when io_uring is not available (writes already completed in _add).
 */
int device_write_batch_submit(struct device *dev);

/* ========================================================================
 * Batch Read API — io_uring accelerated
 * ======================================================================== */

int device_read_batch_begin(struct device *dev);

int device_read_batch_add(struct device *dev, uint64_t offset, void *buf,
                          size_t size);

int device_read_batch_submit(struct device *dev);

#endif /* DEVICE_IO_H */
