/*
 * device_io.c — Low-level device I/O implementation
 */

#define _GNU_SOURCE
#include "device_io.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h> /* BLKGETSIZE64 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

int device_open(struct device *dev, const char *path, int read_only) {
  memset(dev, 0, sizeof(*dev));
  strncpy(dev->path, path, sizeof(dev->path) - 1);
  dev->read_only = read_only;

  int flags = read_only ? O_RDONLY : O_RDWR;
  /* Use O_DIRECT if possible for safety, fall back if not */
  dev->fd = open(path, flags);
  if (dev->fd < 0) {
    fprintf(stderr, "btrfs2ext4: cannot open %s: %s\n", path, strerror(errno));
    return -1;
  }

  /* Determine device/file size */
  struct stat st;
  if (fstat(dev->fd, &st) < 0) {
    fprintf(stderr, "btrfs2ext4: cannot stat %s: %s\n", path, strerror(errno));
    close(dev->fd);
    return -1;
  }

  if (S_ISBLK(st.st_mode)) {
    /* Block device — use ioctl to get size */
    uint64_t size;
    if (ioctl(dev->fd, BLKGETSIZE64, &size) < 0) {
      fprintf(stderr, "btrfs2ext4: cannot get size of %s: %s\n", path,
              strerror(errno));
      close(dev->fd);
      return -1;
    }
    dev->size = size;
  } else if (S_ISREG(st.st_mode)) {
    /* Regular file (loopback image) */
    dev->size = (uint64_t)st.st_size;
  } else {
    fprintf(stderr, "btrfs2ext4: %s is not a block device or regular file\n",
            path);
    close(dev->fd);
    return -1;
  }

  if (dev->size == 0) {
    fprintf(stderr, "btrfs2ext4: %s has zero size\n", path);
    close(dev->fd);
    return -1;
  }

  return 0;
}

void device_close(struct device *dev) {
  if (dev->fd >= 0) {
    fsync(dev->fd);
    close(dev->fd);
    dev->fd = -1;
  }
}

int device_read(struct device *dev, uint64_t offset, void *buf, size_t size) {
  if (size > dev->size || offset > dev->size - size) {
    fprintf(stderr,
            "btrfs2ext4: read beyond device end: offset=%lu size=%zu "
            "dev_size=%lu\n",
            (unsigned long)offset, size, (unsigned long)dev->size);
    return -1;
  }

  ssize_t total = 0;
  uint8_t *p = (uint8_t *)buf;

  while ((size_t)total < size) {
    ssize_t n = pread(dev->fd, p + total, size - total, offset + total);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "btrfs2ext4: read error at offset %lu: %s\n",
              (unsigned long)(offset + total), strerror(errno));
      return -1;
    }
    if (n == 0) {
      fprintf(stderr, "btrfs2ext4: unexpected EOF at offset %lu\n",
              (unsigned long)(offset + total));
      return -1;
    }
    total += n;
  }

  return 0;
}

int device_write(struct device *dev, uint64_t offset, const void *buf,
                 size_t size) {
  if (dev->read_only) {
    fprintf(stderr,
            "btrfs2ext4: cannot write: device opened read-only (dry-run)\n");
    return -1;
  }

  if (size > dev->size || offset > dev->size - size) {
    fprintf(stderr,
            "btrfs2ext4: write beyond device end: offset=%lu size=%zu "
            "dev_size=%lu\n",
            (unsigned long)offset, size, (unsigned long)dev->size);
    return -1;
  }

  ssize_t total = 0;
  const uint8_t *p = (const uint8_t *)buf;

  while ((size_t)total < size) {
    ssize_t n = pwrite(dev->fd, p + total, size - total, offset + total);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "btrfs2ext4: write error at offset %lu: %s\n",
              (unsigned long)(offset + total), strerror(errno));
      return -1;
    }
    total += n;
  }

  return 0;
}

int device_sync(struct device *dev) {
  if (dev->read_only)
    return 0;

  if (fdatasync(dev->fd) < 0) {
    fprintf(stderr, "btrfs2ext4: sync error: %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

uint64_t device_get_size(struct device *dev) { return dev->size; }
