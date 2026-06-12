#ifndef EXT4_SPACE_H
#define EXT4_SPACE_H
#include <stdint.h>
struct ext4_layout;
struct btrfs_fs_info;
struct ext4_space_budget {
  uint64_t data_blocks, journal_blocks, htree_blocks, dedup_blocks;
  uint64_t metadata_reserved, total_required, free_available, margin_blocks;
  uint32_t margin_percent;
};
int ext4_space_budget(const struct ext4_layout *, const struct btrfs_fs_info *,
                      uint64_t, uint32_t, struct ext4_space_budget *);
int ext4_space_budget_validate(const struct ext4_space_budget *);
void ext4_space_budget_print(const struct ext4_space_budget *, uint32_t);
uint32_t ext4_space_margin_from_env(void);
#endif
