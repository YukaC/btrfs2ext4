/*
 * dir_writer.c — Ext4 directory entry writer
 *
 * Creates ext4 directory blocks from the in-memory file/directory tree.
 * Supports multi-block directories for large directories.
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "btrfs/btrfs_reader.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

/*
 * Calculate the actual record length for a directory entry.
 * Must be aligned to 4 bytes.
 */
static uint16_t dir_entry_len(uint8_t name_len) {
  /* Fixed header (8 bytes) + name, rounded up to 4-byte boundary */
  return (uint16_t)((8 + name_len + 3) & ~3);
}

/*
 * Convert btrfs file type to ext4 file type
 */
static uint8_t btrfs_to_ext4_filetype(uint32_t mode) {
  if (S_ISREG(mode))
    return EXT4_FT_REG_FILE;
  if (S_ISDIR(mode))
    return EXT4_FT_DIR;
  if (S_ISCHR(mode))
    return EXT4_FT_CHRDEV;
  if (S_ISBLK(mode))
    return EXT4_FT_BLKDEV;
  if (S_ISFIFO(mode))
    return EXT4_FT_FIFO;
  if (S_ISSOCK(mode))
    return EXT4_FT_SOCK;
  if (S_ISLNK(mode))
    return EXT4_FT_SYMLINK;
  return EXT4_FT_UNKNOWN;
}

/*
 * Write a single directory entry into a block buffer.
 * Returns the number of bytes written, or 0 if it doesn't fit.
 */
static uint32_t write_dir_entry(uint8_t *block, uint32_t offset,
                                uint32_t block_size, uint32_t inode,
                                uint8_t name_len, uint8_t file_type,
                                const char *name) {
  uint16_t entry_len = dir_entry_len(name_len);
  if (offset + entry_len > block_size)
    return 0;

  struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(block + offset);
  de->inode = htole32(inode);
  de->name_len = name_len;
  de->file_type = file_type;
  de->rec_len = htole16(entry_len);
  memcpy(de->name, name, name_len);
  return entry_len;
}

/*
 * Finalize a directory block: make the last entry's rec_len cover
 * the remainder of the block.
 */
static void finalize_dir_block(uint8_t *block, uint32_t used,
                               uint32_t block_size) {
  if (used == 0)
    return;

  /* Walk to find the last entry */
  uint32_t last_offset = 0;
  uint32_t scan = 0;
  while (scan < used) {
    struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(block + scan);
    last_offset = scan;
    uint16_t rl = le16toh(de->rec_len);
    if (rl == 0)
      break;
    scan += rl;
  }

  /* Extend last entry to fill the block */
  struct ext4_dir_entry_2 *last_de =
      (struct ext4_dir_entry_2 *)(block + last_offset);
  last_de->rec_len = htole16((uint16_t)(block_size - last_offset));
}

/*
 * Ext4 Legacy Hash Algorithm for HTree directories
 */
static uint32_t ext4_legacy_hash(const char *name, uint8_t len) {
  uint32_t hash = 0x12a3fe2d, padding = 0x37abe8f9;
  for (int i = 0; i < len; i++) {
    uint32_t p0 = padding;
    padding += hash;
    hash = (hash << 8) | (hash >> 24); /* ROTL 8 */
    hash ^= p0 ^ ((unsigned char)name[i]);
  }
  return hash;
}

static int compare_file_entry_hash(const void *a, const void *b) {
  const struct dir_entry_link *la = (const struct dir_entry_link *)a;
  const struct dir_entry_link *lb = (const struct dir_entry_link *)b;
  uint32_t ha = ext4_legacy_hash(la->name, (uint8_t)la->name_len);
  uint32_t hb = ext4_legacy_hash(lb->name, (uint8_t)lb->name_len);
  if (ha < hb)
    return -1;
  if (ha > hb)
    return 1;
  return 0;
}

int ext4_write_directories(struct device *dev, const struct ext4_layout *layout,
                           const struct btrfs_fs_info *fs_info,
                           const struct inode_map *inode_map,
                           struct ext4_block_allocator *alloc) {
  uint32_t block_size = layout->block_size;

  printf("Writing directory entries...\n");

  /* For each directory in the filesystem */
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    const struct file_entry *dir = fs_info->inode_table[i];
    if (!S_ISDIR(dir->mode))
      continue;

    uint32_t dir_ino = inode_map_lookup(inode_map, dir->ino);
    uint32_t parent_ino;
    if (dir->ino == BTRFS_FIRST_FREE_OBJECTID) {
      parent_ino = EXT4_ROOT_INO;
    } else {
      parent_ino = inode_map_lookup(inode_map, dir->parent_ino);
      if (parent_ino == 0)
        parent_ino = EXT4_ROOT_INO;
    }

    if (dir_ino == 0)
      continue;

    /*
     * Build directory blocks.
     * Support for multi-block directories via Ext4 HTree index (EXT4_INDEX_FL).
     */
    uint32_t dir_size = 24;
    for (uint32_t c = 0; c < dir->child_count; c++) {
      uint8_t nl = (uint8_t)dir->children[c].name_len;
      if (nl > 0)
        dir_size += dir_entry_len(nl);
    }

    int use_htree = (dir_size > block_size);
    if (use_htree) {
      /* Signal to inode_writer that this directory needs EXT4_INDEX_FL */
      ((struct file_entry *)dir)->ext4_flags |= EXT4_INDEX_FL;
      qsort(((struct file_entry *)dir)->children, dir->child_count,
            sizeof(struct dir_entry_link), compare_file_entry_hash);
    }

    /* Max ~260,000 blocks per directory (v1 2-Level HTree)
     * Start with a reasonable allocation and grow if needed. */
    uint32_t max_dir_blocks = use_htree ? (dir_size / block_size + 10) : 1;
    if (max_dir_blocks < 4)
      max_dir_blocks = 4;

    uint64_t *dir_block_nums = calloc(max_dir_blocks, sizeof(uint64_t));
    uint8_t **dir_blocks = calloc(max_dir_blocks, sizeof(uint8_t *));
    if (!dir_block_nums || !dir_blocks) {
      free(dir_block_nums);
      free(dir_blocks);
      return -1;
    }

    uint32_t num_blocks = 0;
    uint32_t offset = 0;

    /* Allocate block 0 */
    dir_blocks[0] = calloc(1, block_size);
    dir_block_nums[0] = ext4_alloc_block(alloc, layout);
    if (!dir_blocks[0] || dir_block_nums[0] == (uint64_t)-1) {
      free(dir_blocks[0]);
      free(dir_blocks);
      free(dir_block_nums);
      fprintf(stderr, "btrfs2ext4: no space for dir block (ino %u)\n", dir_ino);
      return -1;
    }
    num_blocks = 1;

    struct ext4_dx_entry *root_entries = NULL;
    uint32_t root_count = 0;
    struct ext4_dx_countlimit *root_limit = NULL;

    struct ext4_dx_entry *node_entries = NULL;
    uint32_t node_count = 0;
    struct ext4_dx_countlimit *node_limit = NULL;
    uint32_t current_node_block = 0;

    if (use_htree) {
      /* Block 0 is the HTree root */
      struct ext4_dir_entry_2 *dot = (void *)dir_blocks[0];
      dot->inode = htole32(dir_ino);
      dot->rec_len = htole16(12);
      dot->name_len = 1;
      dot->file_type = EXT4_FT_DIR;
      dot->name[0] = '.';

      struct ext4_dir_entry_2 *dotdot = (void *)(dir_blocks[0] + 12);
      dotdot->inode = htole32(parent_ino);
      dotdot->rec_len = htole16(block_size - 12);
      dotdot->name_len = 2;
      dotdot->file_type = EXT4_FT_DIR;
      dotdot->name[0] = '.';
      dotdot->name[1] = '.';

      struct ext4_dx_root_info *info = (void *)(dir_blocks[0] + 24);
      info->hash_version =
          EXT4_HASH_HALF_MD4; /* Must match sb.s_def_hash_version */
      info->info_length = 8;
      info->indirect_levels = 1; /* 2-level HTree */
      info->unused_flags = 0;

      root_limit = (void *)(dir_blocks[0] + 32);
      root_limit->limit =
          htole16((block_size - 32) / sizeof(struct ext4_dx_entry));
      root_limit->count = htole16(0);

      root_entries = (void *)(dir_blocks[0] + 40);

      /* Spawn the first Node Block (Block 1) */
      current_node_block = 1;
      dir_blocks[1] = calloc(1, block_size);
      dir_block_nums[1] = ext4_alloc_block(alloc, layout);
      num_blocks = 2;

      struct ext4_dir_entry_2 *nf = (void *)dir_blocks[1];
      nf->inode = 0;
      nf->rec_len = htole16(block_size);
      nf->name_len = 0;
      nf->file_type = 0;

      node_limit = (void *)(dir_blocks[1] + 8);
      node_limit->limit =
          htole16((block_size - 16) / sizeof(struct ext4_dx_entry));
      node_limit->count = htole16(0);

      node_entries = (void *)(dir_blocks[1] + 16);
      node_count = 0;

      /* Register Node Block in Root Block */
      root_entries[0].hash = 0;
      root_entries[0].block = htole32(1);
      root_count = 1;
      root_limit->count = htole16(root_count);

      /* Spawn the first Leaf Block (Block 2) */
      dir_blocks[2] = calloc(1, block_size);
      dir_block_nums[2] = ext4_alloc_block(alloc, layout);
      num_blocks = 3;
      offset = 0;

      /* Register Leaf Block in Node Block */
      node_entries[0].hash = 0;
      node_entries[0].block = htole32(2);
      node_count = 1;
      node_limit->count = htole16(node_count);

    } else {
      /* Linear directory Block 0 */
      uint32_t written = write_dir_entry(dir_blocks[0], offset, block_size,
                                         dir_ino, 1, EXT4_FT_DIR, ".");
      offset += written;
      written = write_dir_entry(dir_blocks[0], offset, block_size, parent_ino,
                                2, EXT4_FT_DIR, "..");
      offset += written;
    }

    /* Write child entries */
    for (uint32_t c = 0; c < dir->child_count; c++) {
      const struct dir_entry_link *link = &dir->children[c];
      const struct file_entry *child = link->target;
      uint32_t child_ino = inode_map_lookup(inode_map, child->ino);
      if (child_ino == 0)
        continue;

      uint8_t name_len = (uint8_t)link->name_len;
      if (name_len == 0)
        continue;

      uint16_t entry_len = dir_entry_len(name_len);

      if (offset + entry_len > block_size) {
        finalize_dir_block(dir_blocks[num_blocks - 1], offset, block_size);

        if (num_blocks + 2 >= max_dir_blocks) {
          uint32_t new_max = max_dir_blocks * 2;
          /* Bug I fix: Use temp variables for realloc to avoid dangling
           * pointers if one succeeds and the other fails. */
          uint64_t *new_nums =
              realloc(dir_block_nums, new_max * sizeof(uint64_t));
          if (!new_nums) {
            fprintf(stderr,
                    "btrfs2ext4: OOM reallocating directory pointer array\n");
            break;
          }
          dir_block_nums = new_nums;

          uint8_t **new_blks = realloc(dir_blocks, new_max * sizeof(uint8_t *));
          if (!new_blks) {
            fprintf(stderr,
                    "btrfs2ext4: OOM reallocating directory block array\n");
            break;
          }
          dir_blocks = new_blks;

          /* Need to safely zero out the new trailing portion of the array */
          for (uint32_t i = max_dir_blocks; i < new_max; i++) {
            dir_blocks[i] = NULL;
            dir_block_nums[i] = 0;
          }
          max_dir_blocks = new_max;
        }

        uint32_t h = use_htree ? ext4_legacy_hash(link->name, name_len) : 0;

        if (use_htree && node_count >= le16toh(node_limit->limit)) {
          /* Node block is full, spawn a new Node Block! */
          if (root_count >= le16toh(root_limit->limit)) {
            fprintf(stderr,
                    "btrfs2ext4: error: dir inode %u exceeds massive 2-level "
                    "HTree limit\n",
                    dir_ino);
            for (uint32_t j = 0; j < num_blocks; j++)
              free(dir_blocks[j]);
            free(dir_blocks);
            free(dir_block_nums);
            return -1;
          }

          current_node_block = num_blocks;
          dir_blocks[current_node_block] = calloc(1, block_size);
          dir_block_nums[current_node_block] = ext4_alloc_block(alloc, layout);
          num_blocks++;

          struct ext4_dir_entry_2 *nf = (void *)dir_blocks[current_node_block];
          nf->inode = 0;
          nf->rec_len = htole16(block_size);
          nf->name_len = 0;
          nf->file_type = 0;
          node_limit = (void *)(dir_blocks[current_node_block] + 8);
          node_limit->limit =
              htole16((block_size - 16) / sizeof(struct ext4_dx_entry));
          node_limit->count = htole16(0);
          node_entries = (void *)(dir_blocks[current_node_block] + 16);
          node_count = 0;

          /* Add Node Block to Root */
          root_entries[root_count].hash = htole32(h);
          root_entries[root_count].block = htole32(current_node_block);
          root_count++;
          root_limit->count = htole16(root_count);
        }

        dir_blocks[num_blocks] = calloc(1, block_size);
        dir_block_nums[num_blocks] = ext4_alloc_block(alloc, layout);
        if (!dir_blocks[num_blocks] ||
            dir_block_nums[num_blocks] == (uint64_t)-1) {
          fprintf(stderr, "btrfs2ext4: no space for directory leaf block\n");
          break;
        }

        if (use_htree) {
          /* Add this leaf block to the current node index */
          node_entries[node_count].hash = htole32(h);
          node_entries[node_count].block = htole32(num_blocks);
          node_count++;
          node_limit->count = htole16(node_count);
        }
        num_blocks++;
        offset = 0;
      }

      uint32_t written = write_dir_entry(
          dir_blocks[num_blocks - 1], offset, block_size, child_ino, name_len,
          btrfs_to_ext4_filetype(child->mode), link->name);
      offset += written;
    }

    /* Finalize last block */
    finalize_dir_block(dir_blocks[num_blocks - 1], offset, block_size);

    /* Bug J fix: Write contiguous directory blocks in a single I/O.
     * If allocator placed them sequentially, one device_write replaces
     * potentially hundreds of 4KB pwrite() calls. */
    int contiguous = 1;
    for (uint32_t b = 1; b < num_blocks; b++) {
      if (dir_block_nums[b] != dir_block_nums[b - 1] + 1) {
        contiguous = 0;
        break;
      }
    }

    if (contiguous && num_blocks > 1) {
      uint8_t *combined = malloc((size_t)num_blocks * block_size);
      if (combined) {
        for (uint32_t b = 0; b < num_blocks; b++)
          memcpy(combined + (size_t)b * block_size, dir_blocks[b], block_size);
        if (device_write(dev, dir_block_nums[0] * block_size, combined,
                         (size_t)num_blocks * block_size) < 0) {
          free(combined);
          for (uint32_t j = 0; j < num_blocks; j++)
            free(dir_blocks[j]);
          free(dir_blocks);
          free(dir_block_nums);
          return -1;
        }
        free(combined);
      } else {
        /* OOM fallback: write per-block with batch API */
        goto write_per_block;
      }
    } else {
    write_per_block:
      /* Use batch API: queue all blocks, submit in one syscall */
      device_write_batch_begin(dev);
      for (uint32_t b = 0; b < num_blocks; b++) {
        if (device_write_batch_add(dev, dir_block_nums[b] * block_size,
                                   dir_blocks[b], block_size) < 0) {
          for (uint32_t j = 0; j < num_blocks; j++)
            free(dir_blocks[j]);
          free(dir_blocks);
          free(dir_block_nums);
          return -1;
        }
      }
      if (device_write_batch_submit(dev) < 0) {
        for (uint32_t j = 0; j < num_blocks; j++)
          free(dir_blocks[j]);
        free(dir_blocks);
        free(dir_block_nums);
        return -1;
      }
    }

    /*
     * Update the inode's extent tree to point to the directory blocks.
     * We need to find the inode in the table and update its i_block.
     * For now, we do this by writing directly to the inode table on disk.
     */
    uint32_t ino_group = (dir_ino - 1) / layout->inodes_per_group;
    uint32_t ino_local = (dir_ino - 1) % layout->inodes_per_group;

    if (ino_group < layout->num_groups) {
      const struct ext4_bg_layout *bg = &layout->groups[ino_group];
      uint64_t inode_offset = bg->inode_table_start * block_size +
                              (uint64_t)ino_local * layout->inode_size;

      /* Bug K fix: Build inode directly instead of Read-Modify-Write.
       * We construct the directory inode in RAM from scratch, avoiding
       * the device_read() that doubled I/O for every directory. */
      uint8_t *inode_buf = calloc(1, layout->inode_size);
      if (inode_buf) {
        struct ext4_inode *tmp_inode = (struct ext4_inode *)inode_buf;

        /* Set directory inode fields */
        tmp_inode->i_mode = htole16(040755);   /* Directory, rwxr-xr-x */
        tmp_inode->i_links_count = htole16(2); /* . and .. */
        tmp_inode->i_flags = htole32(EXT4_EXTENTS_FL | EXT4_INDEX_FL);

        /* Directory size = num_blocks * block_size */
        uint64_t dir_size = (uint64_t)num_blocks * block_size;
        tmp_inode->i_size_lo = htole32((uint32_t)(dir_size & 0xFFFFFFFF));
        tmp_inode->i_size_high = htole32((uint32_t)(dir_size >> 32));

        /* Block count (in 512-byte sectors) */
        uint64_t sectors = (dir_size + 511) / 512;
        tmp_inode->i_blocks_lo = htole32((uint32_t)(sectors & 0xFFFFFFFF));
        tmp_inode->i_blocks_high = htole16((uint16_t)(sectors >> 32));

        /* Compile blocks into contiguous extents */
        struct _dir_ext {
          uint32_t len;
          uint64_t phys;
        } *exts = calloc(num_blocks, sizeof(*exts));
        uint16_t n_extents = 0;

        if (num_blocks > 0) {
          exts[0].len = 1;
          exts[0].phys = dir_block_nums[0];
          n_extents = 1;
          for (uint32_t b = 1; b < num_blocks; b++) {
            if (dir_block_nums[b] ==
                    exts[n_extents - 1].phys + exts[n_extents - 1].len &&
                exts[n_extents - 1].len < 32768) {
              exts[n_extents - 1].len++;
            } else {
              exts[n_extents].len = 1;
              exts[n_extents].phys = dir_block_nums[b];
              n_extents++;
            }
          }
        }

        uint16_t max_inline = 4;

        if (n_extents <= max_inline) {
          /* Inline extent tree (depth=0) */
          struct ext4_extent_header *eh =
              (struct ext4_extent_header *)tmp_inode->i_block;
          eh->eh_magic = htole16(EXT4_EXT_MAGIC);
          eh->eh_depth = htole16(0);
          eh->eh_entries = htole16(n_extents);
          eh->eh_max = htole16(max_inline);

          struct ext4_extent *ext =
              (struct ext4_extent *)((uint8_t *)tmp_inode->i_block +
                                     sizeof(struct ext4_extent_header));

          uint32_t logical_block = 0;
          for (uint16_t e = 0; e < n_extents; e++) {
            ext[e].ee_block = htole32(logical_block);
            ext[e].ee_len = htole16((uint16_t)exts[e].len);
            ext[e].ee_start_lo = htole32((uint32_t)(exts[e].phys & 0xFFFFFFFF));
            ext[e].ee_start_hi = htole16((uint16_t)(exts[e].phys >> 32));
            logical_block += exts[e].len;
          }
        } else {
          /* Depth=1 extent tree */
          uint64_t leaf_block = ext4_alloc_block(alloc, layout);
          if (leaf_block == (uint64_t)-1) {
            fprintf(stderr, "btrfs2ext4: no space for dir extent tree leaf\n");
            free(exts);
            free(inode_buf);
            goto cleanup;
          }

          struct ext4_extent_header *root_eh =
              (struct ext4_extent_header *)tmp_inode->i_block;
          root_eh->eh_magic = htole16(EXT4_EXT_MAGIC);
          root_eh->eh_depth = htole16(1);
          root_eh->eh_entries = htole16(1);
          root_eh->eh_max = htole16(max_inline);

          struct ext4_extent_idx *idx =
              (struct ext4_extent_idx *)((uint8_t *)tmp_inode->i_block +
                                         sizeof(struct ext4_extent_header));
          idx->ei_block = htole32(0);
          idx->ei_leaf_lo = htole32((uint32_t)(leaf_block & 0xFFFFFFFF));
          idx->ei_leaf_hi = htole16((uint16_t)(leaf_block >> 32));
          idx->ei_unused = 0;

          /* Create leaf block */
          uint8_t *leaf_buf = calloc(1, block_size);
          struct ext4_extent_header *leaf_eh =
              (struct ext4_extent_header *)leaf_buf;
          leaf_eh->eh_magic = htole16(EXT4_EXT_MAGIC);
          leaf_eh->eh_depth = htole16(0);
          leaf_eh->eh_entries = htole16(n_extents);
          leaf_eh->eh_max =
              htole16((block_size - sizeof(struct ext4_extent_header)) /
                      sizeof(struct ext4_extent));

          struct ext4_extent *leaf_ext =
              (struct ext4_extent *)(leaf_buf +
                                     sizeof(struct ext4_extent_header));

          uint32_t logical_block = 0;
          for (uint16_t e = 0; e < n_extents; e++) {
            leaf_ext[e].ee_block = htole32(logical_block);
            leaf_ext[e].ee_len = htole16((uint16_t)exts[e].len);
            leaf_ext[e].ee_start_lo =
                htole32((uint32_t)(exts[e].phys & 0xFFFFFFFF));
            leaf_ext[e].ee_start_hi = htole16((uint16_t)(exts[e].phys >> 32));
            logical_block += exts[e].len;
          }

          if (device_write(dev, leaf_block * block_size, leaf_buf, block_size) <
              0) {
            fprintf(stderr,
                    "btrfs2ext4: failed to write dir extent tree leaf\n");
          }
          free(leaf_buf);

          /* Extra dir block adds to inode block count */
          uint64_t sectors_including_leaf =
              ((dir_size + block_size) + 511) / 512;
          tmp_inode->i_blocks_lo =
              htole32((uint32_t)(sectors_including_leaf & 0xFFFFFFFF));
          tmp_inode->i_blocks_high =
              htole16((uint16_t)(sectors_including_leaf >> 32));
        }

        device_write(dev, inode_offset, inode_buf, layout->inode_size);
        free(exts);
        free(inode_buf);
      }
    }

  cleanup:
    /* Cleanup */
    for (uint32_t b = 0; b < num_blocks; b++)
      free(dir_blocks[b]);
    free(dir_blocks);
    free(dir_block_nums);
  }

  printf("  Directory entries written\n");
  return 0;
}
