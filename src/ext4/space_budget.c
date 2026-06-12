/*
 * space_budget.c — Phase 4 Approach C
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "btrfs/btrfs_reader.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_space.h"
static uint32_t journal_blocks_for_device(uint64_t ds, uint32_t bs) {
  uint64_t mib = ds / (1024 * 1024); uint32_t jm;
  if (mib < 512) jm = 4; else if (mib < 1024) jm = 16; else if (mib < 2048) jm = 32;
  else if (mib < 4096) jm = 64; else jm = 128;
  return (jm * 1024 * 1024) / bs;
}
static uint16_t dir_entry_len(uint8_t nl) { return (uint16_t)((8 + nl + 3) & ~3); }
static uint64_t compute_data_blocks(const struct btrfs_fs_info *fs, uint32_t bs) {
  uint64_t t = 0; if (!fs) return 0;
  for (uint32_t i = 0; i < fs->inode_count; i++) {
    struct file_entry *fe = fs->inode_table[i]; if (!fe) continue;
    if (fe->mode & S_IFLNK) { if (fe->size > 59) t++; }
    else if (fe->mode & S_IFREG) {
      if (fe->extent_count > 4) t += (fe->extent_count + 339) / 340;
      for (uint32_t e = 0; e < fe->extent_count; e++) {
        struct file_extent *ex = &fe->extents[e];
        if (ex->type != BTRFS_FILE_EXTENT_INLINE && ex->type != BTRFS_FILE_EXTENT_PREALLOC && ex->disk_bytenr)
          t += (ex->num_bytes + bs - 1) / bs;
      }
    } else if (fe->mode & S_IFDIR) t += (fe->size + bs - 1) / bs;
  }
  return t;
}
static uint64_t compute_htree_blocks(const struct btrfs_fs_info *fs, uint32_t bs) {
  uint64_t t = 0; if (!fs) return 0;
  for (uint32_t i = 0; i < fs->inode_count; i++) {
    struct file_entry *fe = fs->inode_table[i];
    if (!fe || !(fe->mode & S_IFDIR)) continue;
    uint32_t ds = 24;
    for (uint32_t c = 0; c < fe->child_count; c++) {
      uint8_t nl = (uint8_t)fe->children[c].name_len; if (nl) ds += dir_entry_len(nl);
    }
    if (ds > bs) {
      uint32_t leaf = (ds + bs - 1) / bs, tot = ds / bs + 10; if (tot < 4) tot = 4;
      if (tot > leaf) t += tot - leaf;
    }
  }
  return t;
}
static uint64_t compute_dedup_blocks(const struct btrfs_fs_info *fs, uint32_t bs) {
  if (!fs) return 0;
  uint64_t t = fs->dedup_blocks_needed;
  if (fs->compressed_extent_count)
    t += (fs->total_decompressed_bytes - fs->total_compressed_bytes + bs - 1) / bs;
  return t;
}
uint32_t ext4_space_margin_from_env(void) {
  const char *e = getenv("BTRFS2EXT4_SAFETY_MARGIN"); if (!e || !*e) return 5;
  long v = strtol(e, NULL, 10); if (v < 0) v = 0; if (v > 100) v = 100; return (uint32_t)v;
}
int ext4_space_budget(const struct ext4_layout *layout, const struct btrfs_fs_info *fs,
                      uint64_t ds, uint32_t mp, struct ext4_space_budget *out) {
  if (!layout || !out) return -1; memset(out, 0, sizeof(*out));
  uint32_t bs = layout->block_size ? layout->block_size : 4096;
  out->margin_percent = mp; out->metadata_reserved = layout->reserved_block_count;
  out->data_blocks = compute_data_blocks(fs, bs);
  out->journal_blocks = journal_blocks_for_device(ds, bs);
  out->htree_blocks = compute_htree_blocks(fs, bs);
  out->dedup_blocks = compute_dedup_blocks(fs, bs);
  out->total_required = out->journal_blocks + out->htree_blocks + out->dedup_blocks;
  uint64_t pu = layout->total_blocks > out->metadata_reserved ? layout->total_blocks - out->metadata_reserved : 0;
  out->free_available = out->data_blocks >= pu ? 0 : pu - out->data_blocks;
  if (mp && layout->total_blocks) out->margin_blocks = layout->total_blocks * mp / 100;
  return 0;
}
int ext4_space_budget_validate(const struct ext4_space_budget *b) {
  if (!b) return -1;
  if (b->total_required + b->margin_blocks > b->free_available) {
    fprintf(stderr, "\n[FATAL] btrfs2ext4: Insufficient space for conversion!\n");
    fprintf(stderr, "  Data blocks required:  %lu\n", (unsigned long)b->data_blocks);
    fprintf(stderr, "  Journal reservation:   %lu\n", (unsigned long)b->journal_blocks);
    fprintf(stderr, "  HTree reservation:     %lu\n", (unsigned long)b->htree_blocks);
    fprintf(stderr, "  Dedup/CoW expansion:   %lu\n", (unsigned long)b->dedup_blocks);
    fprintf(stderr, "  Metadata reserved:     %lu\n", (unsigned long)b->metadata_reserved);
    fprintf(stderr, "  Extra blocks required: %lu\n", (unsigned long)b->total_required);
    fprintf(stderr, "  Safety margin (%u%%):   %lu\n", b->margin_percent, (unsigned long)b->margin_blocks);
    fprintf(stderr, "  Free headroom:         %lu\n", (unsigned long)b->free_available);
    return -1;
  }
  return 0;
}
void ext4_space_budget_print(const struct ext4_space_budget *b, uint32_t bs) {
  if (!b || !bs) return;
  printf("  Data blocks required:   %lu blocks (%.1f MiB)\n", (unsigned long)b->data_blocks, (double)(b->data_blocks*bs)/(1024.*1024.));
  printf("  Journal reservation:    %lu blocks (%.1f MiB)\n", (unsigned long)b->journal_blocks, (double)(b->journal_blocks*bs)/(1024.*1024.));
  printf("  HTree reservation:      %lu blocks (%.1f MiB)\n", (unsigned long)b->htree_blocks, (double)(b->htree_blocks*bs)/(1024.*1024.));
  printf("  Dedup/CoW expansion:    %lu blocks (%.1f MiB)\n", (unsigned long)b->dedup_blocks, (double)(b->dedup_blocks*bs)/(1024.*1024.));
  printf("  Metadata reserved:      %lu blocks\n", (unsigned long)b->metadata_reserved);
  printf("  Extra blocks required:  %lu blocks (%.1f MiB)\n", (unsigned long)b->total_required, (double)(b->total_required*bs)/(1024.*1024.));
  printf("  Free headroom:          %lu blocks (%.1f MiB)\n", (unsigned long)b->free_available, (double)(b->free_available*bs)/(1024.*1024.));
  printf("  Safety margin (%u%%):    %lu blocks (%.1f MiB)\n", b->margin_percent, (unsigned long)b->margin_blocks, (double)(b->margin_blocks*bs)/(1024.*1024.));
  if (b->free_available) {
    uint64_t slack = b->free_available - b->total_required - b->margin_blocks;
    printf("  Space viability check:  OK (%.1f%% headroom after margin)\n", (double)slack*100./(double)b->free_available);
  }
}
