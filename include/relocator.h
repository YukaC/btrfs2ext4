/*
 * relocator.h — Block relocation engine
 */

#ifndef RELOCATOR_H
#define RELOCATOR_H

#include <stdint.h>

struct device;
struct ext4_layout;
struct btrfs_fs_info;

/* A single relocation operation */
struct relocation_entry {
  uint64_t src_offset; /* original physical byte offset */
  uint64_t dst_offset; /* new physical byte offset */
  uint64_t length;     /* bytes to move */
  uint32_t checksum;   /* CRC32 of the data */
  uint32_t seq;        /* sequence number (for journal ordering) */
  uint8_t completed;   /* 1 when verified */
};

/* Relocation plan */
struct relocation_plan {
  struct relocation_entry *entries;
  uint32_t count;
  uint32_t capacity;
  uint64_t total_bytes_to_move;
};

/*
 * Build the relocation plan: identify all data blocks that conflict
 * with ext4 metadata positions and find free destinations for them.
 *
 * Returns 0 on success, -1 on error (e.g., not enough free space).
 */
int relocator_plan(struct relocation_plan *plan,
                   const struct ext4_layout *layout,
                   struct btrfs_fs_info *fs_info);

/*
 * Execute the relocation plan: move data blocks and update
 * the in-memory extent maps in fs_info.
 *
 * This is the most critical operation — it physically moves data.
 * Each move is journaled for crash recovery.
 *
 * Returns 0 on success, -1 on error (partial moves are journaled).
 */
int relocator_execute(struct relocation_plan *plan, struct device *dev,
                      struct btrfs_fs_info *fs_info, uint32_t block_size);

/*
 * Free relocation plan resources.
 */
void relocator_free(struct relocation_plan *plan);

#endif /* RELOCATOR_H */
