/*
 * decompress.h — Btrfs extent decompression API
 *
 * Transparent decompression of zlib/LZO/zstd compressed extents.
 */

#ifndef BTRFS_DECOMPRESS_H
#define BTRFS_DECOMPRESS_H

#include <stddef.h>
#include <stdint.h>

struct device;
struct chunk_map;
struct file_extent;

/*
 * Decompress a single Btrfs extent.
 *
 * Reads the compressed data from disk (using chunk_map to resolve the
 * logical address), decompresses it, and returns a newly-allocated buffer
 * containing the decompressed data.
 *
 * Parameters:
 *   dev         - device handle
 *   chunk_map   - for logical→physical address resolution
 *   ext         - the file_extent to decompress (must have compression != 0)
 *   block_size  - filesystem block size (for alignment)
 *   out_buf     - *out_buf is set to the decompressed data (caller frees)
 *   out_len     - set to the decompressed data length
 *
 * Returns 0 on success, -1 on error.
 */
int btrfs_decompress_extent(struct device *dev,
                            const struct chunk_map *chunk_map,
                            const struct file_extent *ext, uint32_t block_size,
                            uint8_t **out_buf, uint64_t *out_len);

#endif /* BTRFS_DECOMPRESS_H */
