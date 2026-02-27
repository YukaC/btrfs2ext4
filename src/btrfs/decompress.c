/*
 * decompress.c — Btrfs extent decompression
 *
 * Reads compressed extent data from disk and decompresses it using the
 * appropriate library (zlib, LZO, or zstd).
 *
 * Btrfs compression format notes:
 * - ZLIB: raw deflate stream (RFC 1950)
 * - LZO:  Btrfs-specific framing — each 4 KiB page is individually
 *         compressed. A 4-byte LE header gives the total compressed size,
 *         followed by per-page segments (4-byte LE len + compressed data).
 * - ZSTD: standard zstd frame
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#ifdef HAVE_LZO
#include <lzo/lzo1x.h>
#endif

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

#include "btrfs/btrfs_reader.h"
#include "btrfs/btrfs_structures.h"
#include "btrfs/chunk_tree.h"
#include "btrfs/decompress.h"
#include "device_io.h"

#define DECOMPRESS_MAX_COMP_SIZE (512ULL * 1024 * 1024)        // 512 MiB
#define DECOMPRESS_MAX_DECOMP_SIZE (4ULL * 1024 * 1024 * 1024) // 4 GiB

static int decompress_zlib(const uint8_t *in, size_t in_len, uint8_t *out,
                           size_t out_len) {
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  strm.next_in = (Bytef *)in;
  strm.avail_in = (uInt)in_len;
  strm.next_out = (Bytef *)out;
  strm.avail_out = (uInt)out_len;

  /* Btrfs uses raw deflate (no zlib/gzip header), windowBits = -15 */
  if (inflateInit2(&strm, -15) != Z_OK) {
    fprintf(stderr, "btrfs2ext4: zlib inflateInit2 failed\n");
    return -1;
  }

  int ret = inflate(&strm, Z_FINISH);
  inflateEnd(&strm);

  if (ret != Z_STREAM_END && ret != Z_OK) {
    fprintf(stderr, "btrfs2ext4: zlib inflate failed (ret=%d)\n", ret);
    return -1;
  }

  return 0;
}

#ifdef HAVE_LZO
static int decompress_lzo(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t out_len) {
  /*
   * Btrfs LZO format:
   *   [4 bytes LE] total compressed length (including this header)
   *   For each page (4096 bytes of decompressed data):
   *     [4 bytes LE] compressed segment length
   *     [N bytes]    LZO1X compressed data
   *
   * We decompress segment-by-segment into the output buffer.
   */
  if (in_len < 4) {
    fprintf(stderr, "btrfs2ext4: LZO data too short\n");
    return -1;
  }

  /* Skip the 4-byte total-length header */
  const uint8_t *p = in + 4;
  const uint8_t *end = in + in_len;
  size_t out_offset = 0;

  while (p < end && out_offset < out_len) {
    if (p + 4 > end)
      break;

    uint32_t seg_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    if (p + seg_len > end) {
      fprintf(stderr, "btrfs2ext4: LZO segment exceeds input\n");
      return -1;
    }

    lzo_uint dst_len = (lzo_uint)(out_len - out_offset);
    int ret =
        lzo1x_decompress_safe(p, seg_len, out + out_offset, &dst_len, NULL);
    if (ret != LZO_E_OK) {
      fprintf(stderr, "btrfs2ext4: LZO decompress failed (ret=%d)\n", ret);
      return -1;
    }

    out_offset += dst_len;
    p += seg_len;
  }

  return 0;
}
#endif /* HAVE_LZO */

#ifdef HAVE_ZSTD
static int decompress_zstd(const uint8_t *in, size_t in_len, uint8_t *out,
                           size_t out_len) {
  size_t ret = ZSTD_decompress(out, out_len, in, in_len);
  if (ZSTD_isError(ret)) {
    fprintf(stderr, "btrfs2ext4: zstd decompress failed: %s\n",
            ZSTD_getErrorName(ret));
    return -1;
  }
  return 0;
}
#endif /* HAVE_ZSTD */

int btrfs_decompress_extent(struct device *dev,
                            const struct chunk_map *chunk_map,
                            const struct file_extent *ext, uint32_t block_size,
                            uint8_t **out_buf, uint64_t *out_len) {
  if (ext->compression == BTRFS_COMPRESS_NONE) {
    /* Not compressed — shouldn't be called, but handle gracefully */
    *out_buf = NULL;
    *out_len = 0;
    return -1;
  }

  uint64_t comp_size = ext->disk_num_bytes;
  uint64_t decomp_size = ext->ram_bytes;
  if (decomp_size == 0)
    decomp_size = ext->num_bytes; /* fallback */

  if (comp_size == 0 || comp_size > DECOMPRESS_MAX_COMP_SIZE) {
    fprintf(stderr,
            "btrfs2ext4: suspicious compressed size %lu bytes "
            "(limit: %lu MiB) — skipping extent\n",
            (unsigned long)comp_size,
            (unsigned long)(DECOMPRESS_MAX_COMP_SIZE / (1024 * 1024)));
    return -1;
  }

  if (decomp_size == 0 || decomp_size > DECOMPRESS_MAX_DECOMP_SIZE) {
    fprintf(stderr,
            "btrfs2ext4: suspicious decompressed size %lu bytes "
            "— skipping extent\n",
            (unsigned long)decomp_size);
    return -1;
  }

  if (comp_size > decomp_size) {
    fprintf(stderr,
            "btrfs2ext4: compressed size > decompressed size "
            "(%lu > %lu) — skipping\n",
            (unsigned long)comp_size, (unsigned long)decomp_size);
    return -1;
  }

  /* Security Check: Limit decompression to max 2x the allocated extent bytes
   * (anti-bomb) */
  if (decomp_size > ext->num_bytes * 2) {
    fprintf(stderr,
            "btrfs2ext4: safety limit exceeded - decompressed size (%lu) > 2x "
            "extent limit (%lu)\n",
            (unsigned long)decomp_size, (unsigned long)ext->num_bytes);
    return -1;
  }

  /* Resolve physical address of compressed data */
  uint64_t phys = chunk_map_resolve(chunk_map, ext->disk_bytenr);
  if (phys == (uint64_t)-1) {
    fprintf(stderr, "btrfs2ext4: cannot resolve compressed extent at 0x%lx\n",
            (unsigned long)ext->disk_bytenr);
    return -1;
  }

  /* Read compressed data from disk */
  static __thread uint8_t *shared_comp_buf = NULL;
  static __thread size_t shared_comp_size = 0;

  if (comp_size > shared_comp_size) {
    free(shared_comp_buf);
    shared_comp_buf = malloc(comp_size);
    if (!shared_comp_buf) {
      shared_comp_size = 0;
      return -1;
    }
    shared_comp_size = comp_size;
  }
  uint8_t *comp_buf = shared_comp_buf;

  static pthread_mutex_t decompress_io_mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&decompress_io_mutex);
  int read_ret = device_read(dev, phys, comp_buf, comp_size);
  pthread_mutex_unlock(&decompress_io_mutex);

  if (read_ret < 0) {
    return -1;
  }

  /* Round up to block boundary */
  uint64_t aligned_size =
      ((decomp_size + block_size - 1) / block_size) * block_size;

  static __thread uint8_t *shared_decomp_buf = NULL;
  static __thread size_t shared_decomp_size = 0;

  if (aligned_size > shared_decomp_size) {
    free(shared_decomp_buf);
    shared_decomp_buf = malloc(aligned_size);
    if (!shared_decomp_buf) {
      shared_decomp_size = 0;
      return -1;
    }
    shared_decomp_size = aligned_size;
  }
  uint8_t *decomp_buf = shared_decomp_buf;
  memset(decomp_buf, 0, aligned_size);

  int ret = -1;

  switch (ext->compression) {
  case BTRFS_COMPRESS_ZLIB:
    ret = decompress_zlib(comp_buf, comp_size, decomp_buf, decomp_size);
    break;

  case BTRFS_COMPRESS_LZO:
#ifdef HAVE_LZO
    ret = decompress_lzo(comp_buf, comp_size, decomp_buf, decomp_size);
#else
    fprintf(stderr,
            "btrfs2ext4: LZO decompression not available (build without "
            "liblzo2)\n");
#endif
    break;

  case BTRFS_COMPRESS_ZSTD:
#ifdef HAVE_ZSTD
    ret = decompress_zstd(comp_buf, comp_size, decomp_buf, decomp_size);
#else
    fprintf(stderr,
            "btrfs2ext4: zstd decompression not available (build without "
            "libzstd)\n");
#endif
    break;

  default:
    fprintf(stderr, "btrfs2ext4: unknown compression type %u\n",
            ext->compression);
    break;
  }

  if (ret < 0) {
    return -1;
  }

  *out_buf = decomp_buf;
  *out_len = decomp_size;
  return 0;
}
