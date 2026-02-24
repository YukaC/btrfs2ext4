/*
 * device_io.h — Low-level device I/O abstraction
 *
 * Provides safe read/write/sync operations on block devices and image files.
 * All operations use absolute byte offsets. Writes are always followed by
 * fdatasync to ensure durability (critical for crash recovery).
 */

#ifndef DEVICE_IO_H
#define DEVICE_IO_H

#include <stddef.h>
#include <stdint.h>

/* Opaque device handle */
struct device {
  int fd;
  uint64_t size;   /* total device/file size in bytes */
  int read_only;   /* 1 = opened read-only (dry-run mode) */
  char path[4096]; /* device path for error messages */
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
 * Automatically calls fdatasync after write.
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

#endif /* DEVICE_IO_H */
