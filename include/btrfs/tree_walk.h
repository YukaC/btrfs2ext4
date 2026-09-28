/*
 * tree_walk.h — Shared Btrfs B-tree walker
 *
 * Provides validated tree traversal with strict error propagation.
 * Used by btree.c, chunk_tree.c, and fs_tree readers.
 */

#ifndef BTRFS_TREE_WALK_H
#define BTRFS_TREE_WALK_H

#include <stdint.h>

struct device;
struct chunk_map;
struct btrfs_disk_key;

/*
 * Callback return values for btrfs_tree_walk().
 * BTRFS_WALK_CONTINUE (0): proceed to next leaf item.
 * BTRFS_WALK_ABORT (-1): stop walk and propagate error to caller.
 */
enum btrfs_walk_error {
  BTRFS_WALK_CONTINUE = 0,
  BTRFS_WALK_ABORT = -1,
};

typedef enum btrfs_walk_error (*btrfs_tree_walk_fn)(
    const struct btrfs_disk_key *key, const void *data, uint32_t data_size,
    void *ctx);

/*
 * Walk a btrfs B-tree, invoking callback for each leaf item.
 * Returns 0 on success, -1 on validation failure or callback abort.
 */
int btrfs_tree_walk(struct device *dev, const struct chunk_map *chunk_map,
                    uint64_t root_logical, uint8_t root_level,
                    uint32_t nodesize, uint16_t csum_type,
                    btrfs_tree_walk_fn callback, void *ctx);

#endif /* BTRFS_TREE_WALK_H */
