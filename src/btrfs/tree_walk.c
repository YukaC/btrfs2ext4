/*
 * tree_walk.c — Shared Btrfs B-tree walker
 *
 * Consolidates checksum, bytenr, level, and bounds validation used by
 * btree.c and chunk_tree.c. Uses a dynamically growing explicit stack.
 */

#include <endian.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_structures.h"
#include "btrfs/checksum.h"
#include "btrfs/chunk_tree.h"
#include "btrfs/tree_walk.h"
#include "device_io.h"

struct walk_stack_entry {
  uint64_t logical;
  uint8_t level;
};

int btrfs_tree_walk(struct device *dev, const struct chunk_map *chunk_map,
                    uint64_t root_logical, uint8_t root_level,
                    uint32_t nodesize, uint16_t csum_type,
                    btrfs_tree_walk_fn callback, void *ctx) {
  posix_fadvise(dev->fd, 0, 0, POSIX_FADV_SEQUENTIAL);

  uint32_t stack_cap = 1024;
  struct walk_stack_entry *stack =
      malloc(stack_cap * sizeof(struct walk_stack_entry));
  if (!stack) {
    fprintf(stderr, "btrfs2ext4: out of memory for btree walk stack\n");
    return -1;
  }
  int stack_top = 0;

  uint8_t *node_buf = malloc(nodesize);
  if (!node_buf) {
    fprintf(stderr, "btrfs2ext4: out of memory for btree node buffer\n");
    free(stack);
    return -1;
  }

  stack[stack_top].logical = root_logical;
  stack[stack_top].level = root_level;
  stack_top++;

  if (root_level > 8) {
    fprintf(stderr,
            "btrfs2ext4: FATAL: tree root level %u is absurdly high "
            "(malicious/corrupt tree?)\n",
            root_level);
    free(stack);
    free(node_buf);
    return -1;
  }

  int ret = 0;

  while (stack_top > 0) {
    stack_top--;
    uint64_t node_logical = stack[stack_top].logical;
    uint8_t expected_level = stack[stack_top].level;

    uint64_t node_physical = chunk_map_resolve(chunk_map, node_logical);
    if (node_physical == (uint64_t)-1) {
      fprintf(stderr,
              "btrfs2ext4: cannot resolve btree node at logical 0x%lx\n",
              (unsigned long)node_logical);
      ret = -1;
      break;
    }

    if (device_read(dev, node_physical, node_buf, nodesize) < 0) {
      ret = -1;
      break;
    }

    const struct btrfs_header *hdr = (const struct btrfs_header *)node_buf;
    uint32_t nritems = le32toh(hdr->nritems);
    uint8_t level = hdr->level;

    if (btrfs_verify_checksum(csum_type, hdr->csum,
                              (const uint8_t *)hdr + BTRFS_CSUM_SIZE,
                              nodesize - BTRFS_CSUM_SIZE) != 0) {
      fprintf(stderr,
              "btrfs2ext4: btree node checksum mismatch at logical 0x%lx "
              "(algorithm: %s)\n",
              (unsigned long)node_logical, btrfs_csum_name(csum_type));
      ret = -1;
      break;
    }

    uint64_t bytenr = le64toh(hdr->bytenr);
    if (bytenr != node_logical) {
      fprintf(
          stderr,
          "btrfs2ext4: btree node bytenr mismatch: expected 0x%lx, got 0x%lx\n",
          (unsigned long)node_logical, (unsigned long)bytenr);
      ret = -1;
      break;
    }

    if (level != expected_level) {
      fprintf(stderr,
              "btrfs2ext4: btree node level mismatch/cycle detected: expected "
              "%u, got %u at 0x%lx\n",
              expected_level, level, (unsigned long)node_logical);
      ret = -1;
      break;
    }

    if (le64toh(hdr->generation) == 0) {
      fprintf(stderr,
              "btrfs2ext4: btree node generation=0 at logical 0x%lx\n",
              (unsigned long)node_logical);
      ret = -1;
      break;
    }

    if (level > 0) {
      const struct btrfs_key_ptr *ptrs =
          (const struct btrfs_key_ptr *)(node_buf +
                                         sizeof(struct btrfs_header));
      uint32_t max_ptrs =
          (nodesize - sizeof(struct btrfs_header)) / sizeof(struct btrfs_key_ptr);
      if (nritems > max_ptrs) {
        fprintf(stderr,
                "btrfs2ext4: btree node nritems=%u exceeds max=%u — corrupt "
                "node at 0x%lx\n",
                nritems, max_ptrs, (unsigned long)node_logical);
        ret = -1;
        break;
      }

      for (uint32_t i = 0; i < nritems; i++) {
        uint64_t child_logical = le64toh(ptrs[i].blockptr);
        uint64_t child_physical = chunk_map_resolve(chunk_map, child_logical);
        if (child_physical != (uint64_t)-1) {
          posix_fadvise(dev->fd, (off_t)child_physical, nodesize,
                        POSIX_FADV_WILLNEED);
        }
      }

      for (int i = (int)nritems - 1; i >= 0; i--) {
        if ((uint32_t)stack_top >= stack_cap - 1) {
          uint32_t new_cap = stack_cap * 2;
          struct walk_stack_entry *new_stack =
              realloc(stack, new_cap * sizeof(struct walk_stack_entry));
          if (!new_stack) {
            fprintf(stderr, "btrfs2ext4: btree walk stack realloc failed\n");
            ret = -1;
            goto done;
          }
          stack = new_stack;
          stack_cap = new_cap;
        }
        stack[stack_top].logical = le64toh(ptrs[i].blockptr);
        stack[stack_top].level = level - 1;
        stack_top++;
      }
    } else {
      const struct btrfs_item *items =
          (const struct btrfs_item *)(node_buf + sizeof(struct btrfs_header));
      uint32_t max_items =
          (nodesize - sizeof(struct btrfs_header)) / sizeof(struct btrfs_item);
      if (nritems > max_items) {
        fprintf(stderr,
                "btrfs2ext4: btree leaf nritems=%u exceeds max=%u — corrupt "
                "node at 0x%lx\n",
                nritems, max_items, (unsigned long)node_logical);
        ret = -1;
        break;
      }

      for (uint32_t i = 0; i < nritems; i++) {
        uint32_t data_offset = le32toh(items[i].offset);
        uint32_t data_size = le32toh(items[i].size);

        const void *data =
            node_buf + sizeof(struct btrfs_header) + data_offset;

        if ((uint64_t)sizeof(struct btrfs_header) + data_offset + data_size >
            nodesize) {
          fprintf(stderr,
                  "btrfs2ext4: btree item data out of bounds in node 0x%lx\n",
                  (unsigned long)node_logical);
          continue;
        }

        enum btrfs_walk_error cb_ret =
            callback(&items[i].key, data, data_size, ctx);
        if (cb_ret == BTRFS_WALK_ABORT) {
          ret = -1;
          goto done;
        }
      }
    }
  }

done:
  free(node_buf);
  free(stack);
  return ret;
}
