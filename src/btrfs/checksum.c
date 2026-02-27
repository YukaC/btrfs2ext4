#include "btrfs/checksum.h"
#include "btrfs/btrfs_structures.h"
#include <endian.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_LIBCRYPTO
#include <openssl/evp.h>
#endif

#ifdef HAVE_LIBXXHASH
#include <xxhash.h>
#endif

static uint32_t crc32c_table[256];
static int crc32c_initialized = 0;

static void crc32c_init_table(void) {
  uint32_t crc;
  for (int i = 0; i < 256; i++) {
    crc = i;
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0x82F63B78;
      else
        crc = (crc >> 1);
    }
    crc32c_table[i] = crc;
  }
  crc32c_initialized = 1;
}

uint32_t btrfs_crc32c(uint32_t crc, const void *data, size_t len) {
  if (!crc32c_initialized) {
    crc32c_init_table();
  }

  const uint8_t *p = data;
  while (len--) {
    crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  }
  // Btrfs CRC32C applies a final bitwise invert before writing to disk,
  // consistent with standard CRC32c (RFC 3720 / iSCSI convention).
  return ~crc;
}

const char *btrfs_csum_name(uint16_t type) {
  switch (type) {
  case BTRFS_CSUM_TYPE_CRC32:
    return "CRC32C";
  case BTRFS_CSUM_TYPE_XXHASH:
    return "xxHash64";
  case BTRFS_CSUM_TYPE_SHA256:
    return "SHA256";
  case BTRFS_CSUM_TYPE_BLAKE2:
    return "BLAKE2b";
  default:
    return "Unknown";
  }
}

size_t btrfs_csum_size(uint16_t type) {
  switch (type) {
  case BTRFS_CSUM_TYPE_CRC32:
    return 4;
  case BTRFS_CSUM_TYPE_XXHASH:
    return 8;
  case BTRFS_CSUM_TYPE_SHA256:
    return 32;
  case BTRFS_CSUM_TYPE_BLAKE2:
    return 32;
  default:
    return 0;
  }
}

int btrfs_verify_checksum(uint16_t type, const uint8_t *stored_csum,
                          const void *data, size_t len) {
  uint8_t computed[32] = {0}; // Max size for BLAKE2b/SHA256 is 32

  switch (type) {
  case BTRFS_CSUM_TYPE_CRC32: {
    // Btrfs CRC32c: seed = ~0U, with final bitwise invert (standard CRC32c).
    // btrfs_crc32c() already applies the final ~crc inversion.
    uint32_t crc = btrfs_crc32c(~0U, data, len);
    uint32_t le_crc = htole32(crc);
    memcpy(computed, &le_crc, 4);
    break;
  }
  case BTRFS_CSUM_TYPE_XXHASH: {
#ifdef HAVE_LIBXXHASH
    // Btrfs uses xxHash64 with seed 0, little-endian on disk
    uint64_t hash = XXH64(data, len, 0);
    uint64_t le_hash = htole64(hash);
    memcpy(computed, &le_hash, 8);
#else
    fprintf(stderr, "error: btrfs2ext4 compiled without xxHash64 support\n");
    return -1;
#endif
    break;
  }
  case BTRFS_CSUM_TYPE_SHA256: {
#ifdef HAVE_LIBCRYPTO
    unsigned int md_len = 0;
    if (!EVP_Digest(data, len, computed, &md_len, EVP_sha256(), NULL)) {
      fprintf(stderr, "error: EVP_Digest() failed for SHA256\n");
      return -1;
    }
#else
    fprintf(stderr,
            "error: btrfs2ext4 compiled without SHA256 (OpenSSL) support\n");
    return -1;
#endif
    break;
  }
  case BTRFS_CSUM_TYPE_BLAKE2: {
#ifdef HAVE_LIBCRYPTO
    unsigned int md_len = 0;
    // OpenSSL uses blake2b256
    if (!EVP_Digest(data, len, computed, &md_len, EVP_blake2b256(), NULL)) {
      fprintf(stderr, "error: EVP_Digest() failed for BLAKE2b-256\n");
      return -1;
    }
#else
    fprintf(stderr,
            "error: btrfs2ext4 compiled without BLAKE2b (OpenSSL) support\n");
    return -1;
#endif
    break;
  }
  default:
    fprintf(stderr, "error: unsupported btrfs csum type %u\n", type);
    return -1;
  }

  size_t size = btrfs_csum_size(type);
  if (size == 0)
    return -1;

  if (memcmp(stored_csum, computed, size) != 0) {
    return -1; // Checksum mismatch
  }

  return 0; // Checksum OK
}

/*
 * Standard RFC 3720 CRC32C wrapper (used by Ext4 and relocation maps).
 * Callers pass an initial CRC (0 for a fresh computation), and the result
 * can be fed back in for chained buffers.  The final invert is already
 * applied inside btrfs_crc32c(), so we just invert the seed here to undo
 * the extra inversion from chaining.
 */
uint32_t crc32c(uint32_t crc, const void *data, size_t len) {
  return btrfs_crc32c(~crc, data, len);
}
