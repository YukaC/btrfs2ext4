#include "ext4/ext4_space.h"

#include "btrfs/btrfs_reader.h"

#include <sys/stat.h>

uint32_t ext4_journal_default_blocks(uint64_t device_size, uint32_t block_size)
{
    uint64_t mib;
    uint32_t journal_mib;

    if (block_size == 0)
        return 0;

    mib = device_size / (1024 * 1024);
    if (mib < 512)
        journal_mib = 4;
    else if (mib < 1024)
        journal_mib = 16;
    else if (mib < 2048)
        journal_mib = 32;
    else if (mib < 4096)
        journal_mib = 64;
    else
        journal_mib = 128;

    return (journal_mib * 1024 * 1024) / block_size;
}

uint16_t ext4_dir_entry_len(uint8_t name_len)
{
    return (uint16_t)((8 + name_len + 3) & ~3u);
}

uint32_t ext4_dir_logical_size(const struct file_entry *dir)
{
    uint32_t dir_size = 24;

    if (!dir)
        return 0;

    for (uint32_t c = 0; c < dir->child_count; c++) {
        uint8_t nl = (uint8_t)dir->children[c].name_len;
        if (nl > 0)
            dir_size += ext4_dir_entry_len(nl);
    }

    return dir_size;
}

uint32_t ext4_htree_blocks_total(const struct file_entry *dir,
                                   uint32_t block_size)
{
    uint32_t dir_size;
    uint32_t htree_total;

    if (!dir || block_size == 0 || !(dir->mode & S_IFDIR))
        return 0;

    dir_size = ext4_dir_logical_size(dir);
    if (dir_size <= block_size)
        return 0;

    htree_total = dir_size / block_size + 10;
    if (htree_total < 4)
        htree_total = 4;

    return htree_total;
}

uint32_t ext4_htree_extra_blocks(const struct file_entry *dir, uint32_t block_size)
{
    uint32_t htree_total = ext4_htree_blocks_total(dir, block_size);
    uint32_t base_blocks;

    if (htree_total == 0)
        return 0;

    base_blocks = (uint32_t)((dir->size + block_size - 1) / block_size);
    if (htree_total <= base_blocks)
        return 0;
    return htree_total - base_blocks;
}
