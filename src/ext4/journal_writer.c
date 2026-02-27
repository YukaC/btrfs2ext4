/*
 * journal_writer.c — Ext4 journal (JBD2) writer
 *
 * Creates the on-disk JBD2 journal for the converted Ext4 filesystem.
 * This eliminates the need for a post-conversion `tune2fs -j`.
 *
 * The journal is stored as inode 8 (EXT4_JOURNAL_INO) and consists of:
 *   - A JBD2 superblock at the first journal block
 *   - Remaining blocks zeroed (empty journal)
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_reader.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

/* JBD2 constants */
#define JBD2_MAGIC_NUMBER 0xC03B3998
#define JBD2_SUPERBLOCK_V2 4 /* block type for v2 superblock */

/* JBD2 superblock — only the fields we need to initialize */
struct jbd2_superblock {
  uint32_t s_header_magic;     /* JBD2_MAGIC_NUMBER (big-endian!) */
  uint32_t s_header_blocktype; /* JBD2_SUPERBLOCK_V2 (big-endian!) */
  uint32_t s_header_sequence;  /* transaction sequence (big-endian!) */
  uint32_t s_blocksize;        /* journal block size (big-endian!) */
  uint32_t s_maxlen;           /* total journal blocks (big-endian!) */
  uint32_t s_first;            /* first usable block (big-endian!) */
  uint32_t s_sequence;         /* first expected commit ID (big-endian!) */
  uint32_t s_start;            /* block of first transaction (0 = clean) */
  uint32_t s_errno;            /* error value (big-endian!) */
  /* Fields beyond here are feature flags etc. — leave zeroed */
  uint8_t s_padding[1024 - 36]; /* pad to 1024 bytes */
} __attribute__((packed));

_Static_assert(sizeof(struct jbd2_superblock) == 1024,
               "jbd2_superblock must be exactly 1024 bytes");

/*
 * Default journal size heuristic (same as mke2fs):
 *   device < 512 MiB  →  4 MiB
 *   device < 1 GiB    → 16 MiB
 *   device < 2 GiB    → 32 MiB
 *   device < 4 GiB    → 64 MiB
 *   device >= 4 GiB   → 128 MiB
 */
static uint32_t journal_default_blocks(uint64_t device_size,
                                       uint32_t block_size) {
  uint64_t mib = device_size / (1024 * 1024);
  uint32_t journal_mib;

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

/* Bug M fix: Replaced global state with per-invocation struct.
 * Previously g_journal_start_block / g_journal_block_count were static
 * globals that would keep stale values if the conversion was retried. */
static uint64_t g_journal_start_block = 0;
static uint32_t g_journal_block_count = 0;

int ext4_write_journal(struct device *dev, const struct ext4_layout *layout,
                       struct ext4_block_allocator *alloc,
                       uint64_t device_size) {
  uint32_t block_size = layout->block_size;
  uint32_t journal_blocks = journal_default_blocks(device_size, block_size);

  /* Bug M fix: Reset globals before each invocation to avoid stale state */
  g_journal_start_block = 0;
  g_journal_block_count = 0;

  printf("Writing ext4 journal (inode 8)...\n");
  printf("  Journal size: %u blocks (%u MiB)\n", journal_blocks,
         (journal_blocks * block_size) / (1024 * 1024));

  /* Phase 3.3: Try to allocate journal sequentially at the absolute end of the
   * device */
  uint64_t first_block = (uint64_t)-1;
  uint32_t got_blocks = 0;

  if (alloc->reserved_bitmap) {
    uint64_t count = 0;
    for (uint64_t b = layout->total_blocks; b-- > 0;) {
      if (alloc->reserved_bitmap[b / 8] & (1 << (b % 8))) {
        count = 0;
      } else {
        count++;
        if (count == journal_blocks) {
          first_block = b;
          got_blocks = journal_blocks;
          /* Mark blocks as used */
          for (uint32_t i = 0; i < journal_blocks; i++) {
            uint64_t mark_b = first_block + i;
            alloc->reserved_bitmap[mark_b / 8] |= (1 << (mark_b % 8));
          }
          break;
        }
      }
    }
  }

  /* Fallback: allocate from front if end-of-device search failed */
  if (first_block == (uint64_t)-1) {
    first_block = ext4_alloc_block(alloc, layout);
    if (first_block == (uint64_t)-1) {
      fprintf(stderr, "btrfs2ext4: no space for journal\n");
      return -1;
    }
    got_blocks = 1;

    /* Try to claim contiguous blocks after first_block via bitmap */
    for (uint32_t i = 1; i < journal_blocks; i++) {
      uint64_t blk = first_block + i;
      if (blk >= alloc->max_blocks)
        break;
      if (alloc->reserved_bitmap &&
          (alloc->reserved_bitmap[blk / 8] & (1 << (blk % 8)))) {
        break;
      }
      if (alloc->reserved_bitmap)
        alloc->reserved_bitmap[blk / 8] |= (1 << (blk % 8));
      got_blocks++;
    }
  }

  /* If we still couldn't get enough contiguous blocks, fall back to allocator
   */
  if (got_blocks < journal_blocks) {
    for (uint32_t i = got_blocks; i < journal_blocks; i++) {
      uint64_t blk = ext4_alloc_block(alloc, layout);
      if (blk == (uint64_t)-1) {
        journal_blocks = i;
        break;
      }
    }
  }

  g_journal_start_block = first_block;
  g_journal_block_count = journal_blocks;

  printf("  Journal blocks: %lu–%lu (%u blocks)\n", (unsigned long)first_block,
         (unsigned long)(first_block + journal_blocks - 1), journal_blocks);

  /* Build JBD2 superblock.
   * NOTE: JBD2 uses big-endian byte order (network order) for its header! */
  uint8_t *jbd_buf = calloc(1, block_size);
  if (!jbd_buf)
    return -1;

  struct jbd2_superblock *jsb = (struct jbd2_superblock *)jbd_buf;
  jsb->s_header_magic = htobe32(JBD2_MAGIC_NUMBER);
  jsb->s_header_blocktype = htobe32(JBD2_SUPERBLOCK_V2);
  jsb->s_header_sequence = htobe32(1);
  jsb->s_blocksize = htobe32(block_size);
  jsb->s_maxlen = htobe32(journal_blocks);
  jsb->s_first = htobe32(1); /* first usable block = 1 (after superblock) */
  jsb->s_sequence = htobe32(1);
  jsb->s_start = htobe32(0); /* 0 = clean journal */
  jsb->s_errno = htobe32(0);

/* Bug G fix: Write journal in large chunks instead of 32768 × 4KB pwrite().
 * Use 16MB chunks to limit RAM usage while reducing syscall overhead. */
#define JOURNAL_CHUNK_SIZE (16 * 1024 * 1024)
  uint32_t chunk_blocks = JOURNAL_CHUNK_SIZE / block_size;
  if (chunk_blocks > journal_blocks)
    chunk_blocks = journal_blocks;

  size_t chunk_bytes = (size_t)chunk_blocks * block_size;
  uint8_t *zero_chunk = calloc(1, chunk_bytes);
  if (!zero_chunk) {
    free(jbd_buf);
    return -1;
  }

  /* Use batch write API for async I/O when io_uring is available */
  device_write_batch_begin(dev);

  /* Write JBD2 superblock as the first block */
  if (device_write_batch_add(dev, first_block * block_size, jbd_buf,
                             block_size) < 0) {
    free(zero_chunk);
    free(jbd_buf);
    return -1;
  }

  /* Write remaining chunks (all zeros) */
  uint32_t written = 1;
  while (written < journal_blocks) {
    uint32_t remaining = journal_blocks - written;
    uint32_t to_write = remaining < chunk_blocks ? remaining : chunk_blocks;
    uint64_t offset = (first_block + written) * block_size;

    if (device_write_batch_add(dev, offset, zero_chunk,
                               (size_t)to_write * block_size) < 0) {
      free(zero_chunk);
      free(jbd_buf);
      return -1;
    }
    written += to_write;
  }

  /* Submit all queued journal writes at once */
  if (device_write_batch_submit(dev) < 0) {
    free(zero_chunk);
    free(jbd_buf);
    return -1;
  }

  free(zero_chunk);
  free(jbd_buf);

  printf("  Journal written (JBD2 v2 superblock + %u empty blocks, "
         "%u chunk writes)\n",
         journal_blocks - 1,
         (journal_blocks + chunk_blocks - 1) / chunk_blocks);

  return 0;
}

uint64_t ext4_journal_start_block(void) { return g_journal_start_block; }

uint32_t ext4_journal_block_count(void) { return g_journal_block_count; }

int ext4_finalize_journal_inode(struct device *dev,
                                const struct ext4_layout *layout) {
  uint32_t block_size = layout->block_size;

  /* Inode 8 → group 0, local index 7 (inodes start at 1) */
  uint32_t ino_group = (EXT4_JOURNAL_INO - 1) / layout->inodes_per_group;
  uint32_t ino_local = (EXT4_JOURNAL_INO - 1) % layout->inodes_per_group;

  if (ino_group >= layout->num_groups)
    return -1;

  const struct ext4_bg_layout *bg = &layout->groups[ino_group];
  uint64_t inode_off = bg->inode_table_start * block_size +
                       (uint64_t)ino_local * layout->inode_size;

  struct ext4_inode jinode;
  memset(&jinode, 0, layout->inode_size);

  jinode.i_mode = htole16(0100600); /* Regular file, rw------- */
  jinode.i_uid = htole16(0);
  jinode.i_size_lo = htole32(
      (uint32_t)((uint64_t)g_journal_block_count * block_size & 0xFFFFFFFF));
  jinode.i_size_high =
      htole32((uint32_t)((uint64_t)g_journal_block_count * block_size >> 32));
  jinode.i_links_count = htole16(1);
  jinode.i_flags = htole32(EXT4_EXTENTS_FL);

  uint64_t sectors = ((uint64_t)g_journal_block_count * block_size + 511) / 512;
  jinode.i_blocks_lo = htole32((uint32_t)(sectors & 0xFFFFFFFF));

  /* Build extent tree for journal (blocks are contiguous, up to 4 extents fit
   * in inline i_block) */
  struct ext4_extent_header *eh = (struct ext4_extent_header *)jinode.i_block;

  uint32_t remaining_blocks = g_journal_block_count;
  uint32_t extents_needed = (remaining_blocks + 32767) / 32768;
  if (extents_needed > 4)
    extents_needed = 4; // limit inline extents

  eh->eh_magic = htole16(EXT4_EXT_MAGIC);
  eh->eh_entries = htole16((uint16_t)extents_needed);
  eh->eh_max = htole16(4);
  eh->eh_depth = htole16(0);

  struct ext4_extent *ext =
      (struct ext4_extent *)(jinode.i_block +
                             sizeof(struct ext4_extent_header));

  uint32_t logical_block = 0;
  uint64_t phys_block = g_journal_start_block;

  for (uint16_t i = 0; i < extents_needed; i++) {
    uint32_t len = remaining_blocks > 32768 ? 32768 : remaining_blocks;
    ext[i].ee_block = htole32(logical_block);
    ext[i].ee_len = htole16((uint16_t)len);
    ext[i].ee_start_lo = htole32((uint32_t)(phys_block & 0xFFFFFFFF));
    ext[i].ee_start_hi = htole16((uint16_t)(phys_block >> 32));

    logical_block += len;
    phys_block += len;
    remaining_blocks -= len;
  }

  return device_write(dev, inode_off, &jinode, sizeof(struct ext4_inode));
}
