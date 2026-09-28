/*
 * btree.c — Generic btrfs B-tree walker (thin wrapper)
 *
 * Delegates to the shared btrfs_tree_walk() implementation.
 */

#include <stdint.h>

#include "btrfs/btrfs_structures.h"
#include "btrfs/chunk_tree.h"
#include "btrfs/tree_walk.h"
#include "device_io.h"

typedef int (*btree_callback)(const struct btrfs_disk_key *key,
                              const void *data, uint32_t data_size, void *ctx);

int btree_walk(struct device *dev, const struct chunk_map *chunk_map,
               uint64_t root_logical, uint8_t root_level, uint32_t nodesize,
               uint16_t csum_type, btree_callback callback, void *ctx) {
  return btrfs_tree_walk(dev, chunk_map, root_logical, root_level, nodesize,
                         csum_type, (btrfs_tree_walk_fn)callback, ctx);
}
