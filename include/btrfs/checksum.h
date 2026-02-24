#ifndef BTRFS_CHECKSUM_H
#define BTRFS_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

const char *btrfs_csum_name(uint16_t type);
size_t btrfs_csum_size(uint16_t type);

/*
 * Validates a given checksum against a data buffer.
 * `type` is one of BTRFS_CSUM_TYPE_* from btrfs_structures.h.
 * `stored_csum` is precisely `btrfs_csum_size(type)` bytes read from disk.
 * Returns 0 if checksum matches, -1 otherwise.
 */
int btrfs_verify_checksum(uint16_t type, const uint8_t *stored_csum,
                          const void *data, size_t len);

/* Lower-level checksum routines used internally or by ext4 writers */
uint32_t btrfs_crc32c(uint32_t crc, const void *data, size_t len);
uint32_t crc32c(uint32_t crc, const void *data, size_t len);

#endif /* BTRFS_CHECKSUM_H */
