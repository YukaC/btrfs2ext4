#ifndef EXT4_CRC16_H
#define EXT4_CRC16_H

#include <stddef.h>
#include <stdint.h>

/**
 * Compute the CRC16-ANSI (polynomial 0x8005) checksum for a buffer,
 * as required by e2fsprogs for ext4 GDT bg_checksum generation.
 */
uint16_t ext4_crc16(uint16_t crc, const void *buffer, size_t len);

#endif /* EXT4_CRC16_H */
