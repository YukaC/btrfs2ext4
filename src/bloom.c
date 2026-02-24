/*
 * bloom.c — Bloom filter for HDD thrashing prevention
 *
 * A probabilistic data structure that uses minimal RAM (~2MB for 1M items)
 * to quickly reject non-existent inode lookups, avoiding useless disk seeks
 * when the inode hash table is paged to disk via mmap().
 *
 * False positive rate: ~1% with k=7 hash functions and 10 bits per element.
 */

#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_reader.h"

/* Knuth-style multiplicative hash variants */
static inline uint64_t bloom_hash(uint64_t key, uint32_t seed) {
  key ^= seed;
  key *= 0x517cc1b727220a95ULL;
  key ^= key >> 32;
  key *= 0x6c62272e07bb0142ULL;
  key ^= key >> 32;
  return key;
}

int bloom_init(struct bloom_filter *bf, uint64_t expected_items) {
  if (!bf || expected_items == 0)
    return -1;

  /* 10 bits per element → ~1% false positive rate */
  if (expected_items > UINT64_MAX / 10) {
    bf->size_bits = (uint64_t)512 * 1024 * 1024 * 8; /* 512 MiB of bits */
  } else {
    bf->size_bits = expected_items * 10;
  }

  if (bf->size_bits < 1024)
    bf->size_bits = 1024;

  /* Round up to full bytes */
  uint64_t byte_count = (bf->size_bits + 7) / 8;
  if (byte_count > 512ULL * 1024 * 1024) {
    byte_count = 512ULL * 1024 * 1024;
    bf->size_bits = byte_count * 8;
  }

  bf->bits = calloc(1, byte_count);
  if (!bf->bits)
    return -1;

  bf->num_hashes = 7; /* optimal k for 10 bits/element */
  return 0;
}

void bloom_add(struct bloom_filter *bf, uint64_t key) {
  for (uint32_t i = 0; i < bf->num_hashes; i++) {
    uint64_t h = bloom_hash(key, i) % bf->size_bits;
    bf->bits[h / 8] |= (1 << (h % 8));
  }
}

int bloom_test(const struct bloom_filter *bf, uint64_t key) {
  for (uint32_t i = 0; i < bf->num_hashes; i++) {
    uint64_t h = bloom_hash(key, i) % bf->size_bits;
    if (!(bf->bits[h / 8] & (1 << (h % 8))))
      return 0; /* definitely not present */
  }
  return 1; /* probably present */
}

void bloom_free(struct bloom_filter *bf) {
  if (bf) {
    free(bf->bits);
    bf->bits = NULL;
    bf->size_bits = 0;
  }
}
