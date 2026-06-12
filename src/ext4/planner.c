/*
 * planner.c — Ext4 layout planner
 *
 * Calculates the ext4 block group layout for a given device size.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/chunk_tree.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_space.h"
#include "ext4/ext4_structures.h"

static uint8_t normalize_safety_margin(uint8_t safety_margin_percent)
{
  if (safety_margin_percent == 0)
    return EXT4_DEFAULT_SAFETY_MARGIN_PERCENT;
  if (safety_margin_percent < EXT4_MIN_SAFETY_MARGIN_PERCENT)
    return EXT4_MIN_SAFETY_MARGIN_PERCENT;
  if (safety_margin_percent > EXT4_MAX_SAFETY_MARGIN_PERCENT)
    return EXT4_MAX_SAFETY_MARGIN_PERCENT;
  return safety_margin_percent;
}

static int reserved_contains(const struct ext4_layout *layout, uint64_t block)
{
  uint32_t lo = 0;
  uint32_t hi = layout->reserved_block_count;

  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (layout->reserved_blocks[mid] < block)
      lo = mid + 1;
    else
      hi = mid;
  }

  return lo < layout->reserved_block_count &&
         layout->reserved_blocks[lo] == block;
}

static int reserve_block(struct ext4_layout *layout, uint64_t block)
{
  if (block >= layout->total_blocks)
    return 0;

  if (layout->reserved_block_count > 0) {
    uint64_t last = layout->reserved_blocks[layout->reserved_block_count - 1];
    if (block == last)
      return 0;
    if (block < last && reserved_contains(layout, block))
      return 0;
  }

  if (layout->reserved_block_count >= layout->reserved_block_capacity) {
    uint32_t new_cap = layout->reserved_block_capacity * 2;
    uint64_t *new_blocks =
        realloc(layout->reserved_blocks, new_cap * sizeof(uint64_t));
    if (!new_blocks)
      return -1;
    layout->reserved_blocks = new_blocks;
    layout->reserved_block_capacity = new_cap;
  }

  layout->reserved_blocks[layout->reserved_block_count++] = block;

  /* Keep reserved_blocks sorted for O(log n) duplicate checks on outliers. */
  if (layout->reserved_block_count > 1 &&
      block < layout->reserved_blocks[layout->reserved_block_count - 2]) {
    uint32_t i = layout->reserved_block_count - 1;
    while (i > 0 && layout->reserved_blocks[i - 1] > block) {
      layout->reserved_blocks[i] = layout->reserved_blocks[i - 1];
      i--;
    }
    layout->reserved_blocks[i] = block;
  }

  return 0;
}

static int reserve_journal_at_tail(struct ext4_layout *layout,
                                   uint64_t device_size)
{
  uint32_t journal_blocks;
  uint64_t start;

  journal_blocks = ext4_journal_default_blocks(device_size, layout->block_size);
  if (journal_blocks == 0 || journal_blocks > layout->total_blocks)
    return -1;

  start = layout->total_blocks - journal_blocks;
  layout->journal_blocks = journal_blocks;
  layout->journal_start_block = start;

  for (uint64_t b = start; b < layout->total_blocks; b++) {
    if (reserve_block(layout, b) < 0)
      return -1;
  }

  return 0;
}

static void planner_fail_cleanup(struct ext4_layout *layout)
{
  free(layout->groups);
  free(layout->reserved_blocks);
  layout->groups = NULL;
  layout->reserved_blocks = NULL;
  layout->reserved_block_count = 0;
  layout->reserved_block_capacity = 0;
}

static void fill_budget(struct ext4_space_budget *budget,
                        uint32_t data_blocks, uint32_t journal_blocks,
                        uint32_t htree_blocks, uint32_t dedup_blocks,
                        uint32_t metadata_blocks, uint32_t margin_blocks)
{
  budget->data_blocks = data_blocks;
  budget->journal_blocks = journal_blocks;
  budget->htree_blocks = htree_blocks;
  budget->dedup_blocks = dedup_blocks;
  budget->metadata_blocks = metadata_blocks;
  budget->safety_margin_blocks = margin_blocks;
  budget->total_required =
      data_blocks + journal_blocks + htree_blocks + dedup_blocks;
}

static uint32_t compute_data_blocks(const struct btrfs_fs_info *fs_info,
                                    uint32_t block_size)
{
  uint32_t data_blocks_required = 0;

  if (!fs_info)
    return 0;

  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    struct file_entry *fe = fs_info->inode_table[i];
    if (!fe)
      continue;

    if (fe->mode & S_IFLNK) {
      if (fe->size > 59)
        data_blocks_required++;
    } else if (fe->mode & S_IFREG) {
      if (fe->extent_count > 4)
        data_blocks_required += (fe->extent_count + 339) / 340;

      for (uint32_t e = 0; e < fe->extent_count; e++) {
        struct file_extent *ext = &fe->extents[e];
        if (ext->type != BTRFS_FILE_EXTENT_INLINE &&
            ext->type != BTRFS_FILE_EXTENT_PREALLOC && ext->disk_bytenr != 0) {
          data_blocks_required +=
              (uint32_t)((ext->num_bytes + block_size - 1) / block_size);
        }
      }
    } else if (fe->mode & S_IFDIR) {
      data_blocks_required +=
          (uint32_t)((fe->size + block_size - 1) / block_size);
    }
  }

  return data_blocks_required;
}

static uint32_t compute_htree_blocks(const struct btrfs_fs_info *fs_info,
                                     uint32_t block_size)
{
  uint32_t htree_blocks = 0;

  if (!fs_info)
    return 0;

  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    struct file_entry *fe = fs_info->inode_table[i];
    if (!fe)
      continue;
    htree_blocks += ext4_htree_extra_blocks(fe, block_size);
  }

  return htree_blocks;
}

static uint32_t compute_dedup_blocks(const struct btrfs_fs_info *fs_info,
                                     uint32_t block_size)
{
  uint64_t dedup = 0;

  if (!fs_info)
    return 0;

  dedup = fs_info->dedup_blocks_needed;
  if (fs_info->compressed_extent_count > 0) {
    uint64_t expansion =
        fs_info->total_decompressed_bytes - fs_info->total_compressed_bytes;
    dedup += (expansion + block_size - 1) / block_size;
  }

  return (uint32_t)dedup;
}

int ext4_plan_layout(const struct btrfs_fs_info *fs_info, uint64_t device_size,
                     uint32_t block_size, uint32_t inode_ratio,
                     uint8_t safety_margin_percent, struct ext4_layout *layout,
                     struct ext4_space_budget *budget) {
  uint32_t data_blocks_required;
  uint32_t htree_blocks;
  uint32_t journal_blocks;
  uint32_t dedup_blocks;
  uint32_t margin_blocks;
  uint64_t physically_usable;
  uint64_t total_required;
  uint64_t free_blocks;
  uint8_t margin_pct;

  memset(layout, 0, sizeof(*layout));

  if (block_size == 0)
    block_size = EXT4_DEFAULT_BLOCK_SIZE;
  if (inode_ratio == 0)
    inode_ratio = EXT4_DEFAULT_INODE_RATIO;

  margin_pct = normalize_safety_margin(safety_margin_percent);
  layout->safety_margin_percent = margin_pct;

  if (device_size == 0 || device_size < block_size) {
    fprintf(stderr, "btrfs2ext4: device too small (%lu bytes)\n",
            (unsigned long)device_size);
    return -1;
  }

  layout->block_size = block_size;

  if (__builtin_mul_overflow(device_size, 1, &physically_usable) ||
      device_size / block_size == 0) {
    fprintf(stderr, "btrfs2ext4: block calculation overflow or zero blocks\n");
    return -1;
  }
  layout->total_blocks = device_size / block_size;
  layout->blocks_per_group = 8 * block_size;
  layout->inode_size = EXT4_DEFAULT_INODE_SIZE;
  layout->desc_size = 64;

  layout->num_groups = (layout->total_blocks + layout->blocks_per_group - 1) /
                       layout->blocks_per_group;

  uint64_t total_inodes_raw = device_size / inode_ratio;

  if (fs_info && total_inodes_raw < fs_info->inode_count + 16)
    total_inodes_raw = fs_info->inode_count + 16;

  layout->inodes_per_group =
      (total_inodes_raw + layout->num_groups - 1) / layout->num_groups;
  layout->inodes_per_group = (layout->inodes_per_group + 7) & ~7U;
  if (layout->inodes_per_group > 8 * block_size)
    layout->inodes_per_group = 8 * block_size;
  if (layout->inodes_per_group < 16)
    layout->inodes_per_group = 16;

  layout->total_inodes = layout->inodes_per_group * layout->num_groups;

  if (fs_info && layout->total_inodes < fs_info->inode_count + 16) {
    fprintf(stderr,
            "\n[FATAL] btrfs2ext4: Architecture Limitation Exceeded!\n");
    fprintf(stderr,
            "This filesystem has %u inodes, but the physical Ext4 "
            "geometry at this device size can only support %u inodes.\n",
            fs_info->inode_count, layout->total_inodes);
    return -1;
  }

  data_blocks_required = compute_data_blocks(fs_info, block_size);
  htree_blocks = compute_htree_blocks(fs_info, block_size);
  journal_blocks = ext4_journal_default_blocks(device_size, block_size);
  dedup_blocks = compute_dedup_blocks(fs_info, block_size);

  printf("=== Ext4 Constraints & Pre-Calculation ===\n");
  printf("  Device size:       %lu bytes (%.1f GiB)\n",
         (unsigned long)device_size,
         (double)device_size / (1024.0 * 1024.0 * 1024.0));
  printf("  Block size:        %u\n", layout->block_size);
  printf("  Total blocks:      %lu\n", (unsigned long)layout->total_blocks);
  printf("  Blocks per group:  %u\n", layout->blocks_per_group);
  printf("  Number of groups:  %u\n", layout->num_groups);
  printf("  Inodes per group:  %u\n", layout->inodes_per_group);
  printf("  Total inodes:      %u\n", layout->total_inodes);
  printf("  Inode size:        %u\n", layout->inode_size);

  layout->groups = calloc(layout->num_groups, sizeof(struct ext4_bg_layout));
  if (!layout->groups) {
    fprintf(stderr, "btrfs2ext4: out of memory for group layouts\n");
    return -1;
  }

  uint32_t gdt_blocks =
      (layout->num_groups * layout->desc_size + block_size - 1) / block_size;

  uint32_t reserved_gdt = 0;
  if (layout->total_blocks > 1024)
    reserved_gdt = gdt_blocks;

  uint32_t inode_table_blocks =
      (layout->inodes_per_group * layout->inode_size + block_size - 1) /
      block_size;

  layout->reserved_block_capacity = 1024;
  layout->reserved_blocks =
      calloc(layout->reserved_block_capacity, sizeof(uint64_t));
  if (!layout->reserved_blocks) {
    free(layout->groups);
    return -1;
  }

  for (uint32_t g = 0; g < layout->num_groups; g++) {
    struct ext4_bg_layout *bg = &layout->groups[g];
    bg->group_start_block = (uint64_t)g * layout->blocks_per_group;

    uint64_t first_block = bg->group_start_block;
    if (g == 0 && block_size > 1024)
      first_block = 0;

    uint64_t cursor = first_block;

    bg->has_super = ext4_bg_has_super(g);

    if (bg->has_super) {
      bg->superblock_block = cursor;
      cursor++;

      bg->gdt_start_block = cursor;
      bg->gdt_blocks = gdt_blocks;
      cursor += gdt_blocks;

      bg->reserved_gdt_blocks = reserved_gdt;
      cursor += reserved_gdt;

      for (uint64_t b = first_block; b < cursor; b++) {
        if (reserve_block(layout, b) < 0) {
          planner_fail_cleanup(layout);
          return -1;
        }
      }
    }

    bg->block_bitmap_block = cursor;
    cursor++;

    bg->inode_bitmap_block = cursor;
    cursor++;

    bg->inode_table_start = cursor;
    bg->inode_table_blocks = inode_table_blocks;
    cursor += inode_table_blocks;

    for (uint64_t b = bg->block_bitmap_block; b < cursor; b++) {
      if (reserve_block(layout, b) < 0) {
        planner_fail_cleanup(layout);
        return -1;
      }
    }

    uint64_t group_end = bg->group_start_block + layout->blocks_per_group;
    if (group_end > layout->total_blocks)
      group_end = layout->total_blocks;

    bg->data_start_block = cursor;
    bg->data_blocks = (cursor < group_end) ? (uint32_t)(group_end - cursor) : 0;
  }

  if (reserve_journal_at_tail(layout, device_size) < 0) {
    fprintf(stderr, "btrfs2ext4: cannot reserve journal at device tail\n");
    planner_fail_cleanup(layout);
    return -1;
  }

  margin_blocks =
      (uint32_t)(layout->total_blocks * (uint64_t)margin_pct / 100);

  printf("  Reserved blocks:   %u (metadata zones + journal)\n",
         layout->reserved_block_count);
  printf("  Data blocks req:   %u (files, index, dirs)\n", data_blocks_required);
  printf("  HTree blocks req:  %u\n", htree_blocks);
  printf("  Journal blocks:    %u (tail %lu–%lu)\n", layout->journal_blocks,
         (unsigned long)layout->journal_start_block,
         (unsigned long)(layout->journal_start_block + layout->journal_blocks -
                         1));
  printf("  Dedup/CoW blocks:  %u\n", dedup_blocks);
  printf("  Safety margin:     %u%% (%u blocks)\n", margin_pct, margin_blocks);

  physically_usable =
      layout->total_blocks > layout->reserved_block_count
          ? layout->total_blocks - layout->reserved_block_count
          : 0;

  total_required = (uint64_t)data_blocks_required + htree_blocks +
                   journal_blocks + dedup_blocks;

  if (total_required >= physically_usable) {
    fprintf(stderr,
            "\n[FATAL] btrfs2ext4: Insufficient space for conversion!\n");
    fprintf(stderr, "  Total blocks:      %lu\n",
            (unsigned long)layout->total_blocks);
    fprintf(stderr, "  Metadata reserved: %u\n", layout->reserved_block_count);
    fprintf(stderr, "  Data to write:     %u\n", data_blocks_required);
    fprintf(stderr, "  HTree overhead:    %u\n", htree_blocks);
    fprintf(stderr, "  Journal:           %u\n", journal_blocks);
    fprintf(stderr, "  Dedup/CoW:         %u\n", dedup_blocks);
    fprintf(stderr, "  Physically usable: %lu\n",
            (unsigned long)physically_usable);
    planner_fail_cleanup(layout);
    return -1;
  }

  free_blocks = physically_usable - total_required;

  if (free_blocks < margin_blocks && margin_blocks > 0) {
    fprintf(stderr, "\n[FATAL] btrfs2ext4: Conversion blocked by safety "
                    "margin rule!\n");
    fprintf(stderr,
            "Calculated free space (%lu MiB) falls below the safety margin of "
            "%u%% (%lu MiB).\n",
            (unsigned long)(free_blocks * block_size) / (1024 * 1024), margin_pct,
            (unsigned long)(margin_blocks * block_size) / (1024 * 1024));
    planner_fail_cleanup(layout);
    return -1;
  }

  printf("  Free Space Margin: %lu blocks (%.1f MiB)\n",
         (unsigned long)free_blocks,
         (double)(free_blocks * block_size) / (1024.0 * 1024.0));
  printf("========================\n\n");

  if (budget) {
    fill_budget(budget, data_blocks_required, journal_blocks, htree_blocks,
                dedup_blocks, layout->reserved_block_count, margin_blocks);
  }

  return 0;
}

uint32_t ext4_find_conflicts(const struct ext4_layout *layout,
                             const struct btrfs_fs_info *fs_info) {
  uint32_t conflicts = 0;
  uint32_t block_size = layout->block_size;

  uint8_t *bitmap = calloc((layout->total_blocks + 7) / 8, 1);
  if (!bitmap) {
    fprintf(stderr, "btrfs2ext4: out of memory allocating conflict bitmap\n");
    return 0;
  }

  for (uint32_t i = 0; i < layout->reserved_block_count; i++) {
    uint64_t b = layout->reserved_blocks[i];
    if (b < layout->total_blocks)
      bitmap[b / 8] |= (1 << (b % 8));
  }

  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    const struct file_entry *fe = fs_info->inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      const struct file_extent *ext = &fe->extents[j];
      if (ext->type == BTRFS_FILE_EXTENT_INLINE ||
          ext->type == BTRFS_FILE_EXTENT_PREALLOC || ext->disk_bytenr == 0)
        continue;

      uint64_t phys = chunk_map_resolve(fs_info->chunk_map, ext->disk_bytenr);
      if (phys == (uint64_t)-1)
        continue;

      uint64_t start_block = phys / block_size;
      uint64_t end_block =
          (phys + ext->disk_num_bytes + block_size - 1) / block_size;

      for (uint64_t b = start_block; b < end_block; b++) {
        if (b < layout->total_blocks && (bitmap[b / 8] & (1 << (b % 8)))) {
          conflicts++;
          break;
        }
      }
    }
  }

  free(bitmap);
  printf("Found %u data extents conflicting with ext4 metadata zones\n\n",
         conflicts);
  return conflicts;
}

void ext4_free_layout(struct ext4_layout *layout) {
  free(layout->groups);
  free(layout->reserved_blocks);
  memset(layout, 0, sizeof(*layout));
}
