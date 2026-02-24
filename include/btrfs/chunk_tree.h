/*
 * chunk_tree.h — Btrfs chunk tree logical-to-physical address resolver
 */

#ifndef CHUNK_TREE_H
#define CHUNK_TREE_H

#include <stdint.h>

/* A single chunk mapping entry */
struct chunk_mapping {
  uint64_t logical;  /* start logical address */
  uint64_t physical; /* start physical address (for devid 1) */
  uint64_t length;   /* length of this chunk */
  uint64_t type;     /* BTRFS_BLOCK_GROUP_* flags */
};

/* Chunk map: array of mappings sorted by logical address */
struct chunk_map {
  struct chunk_mapping *entries;
  uint32_t count;
  uint32_t capacity;
};

struct device;
struct btrfs_super_block;

/*
 * Initialize chunk map from the superblock's sys_chunk_array.
 * This provides the bootstrap mappings needed to read the chunk tree itself.
 * Returns 0 on success, -1 on error.
 */
int chunk_map_init_from_superblock(struct chunk_map *map,
                                   const struct btrfs_super_block *sb);

/*
 * Fully populate the chunk map by walking the chunk tree.
 * Requires bootstrap mappings from chunk_map_init_from_superblock().
 * Returns 0 on success, -1 on error.
 */
int chunk_map_populate(struct chunk_map *map, struct device *dev,
                       const struct btrfs_super_block *sb);

/*
 * Resolve a logical address to a physical address.
 * Returns the physical byte offset, or (uint64_t)-1 on failure.
 */
uint64_t chunk_map_resolve(const struct chunk_map *map, uint64_t logical);

/*
 * Free chunk map resources.
 */
void chunk_map_free(struct chunk_map *map);

#endif /* CHUNK_TREE_H */
