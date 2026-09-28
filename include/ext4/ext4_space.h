#ifndef EXT4_SPACE_H
#define EXT4_SPACE_H

#include <stdint.h>

struct file_entry;

uint32_t ext4_journal_default_blocks(uint64_t device_size, uint32_t block_size);
uint16_t ext4_dir_entry_len(uint8_t name_len);
uint32_t ext4_dir_logical_size(const struct file_entry *dir);
uint32_t ext4_htree_blocks_total(const struct file_entry *dir,
                                   uint32_t block_size);
uint32_t ext4_htree_extra_blocks(const struct file_entry *dir, uint32_t block_size);

#endif
