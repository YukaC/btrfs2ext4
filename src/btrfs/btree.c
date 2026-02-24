/*
 * btree.c — Generic btrfs B-tree walker
 *
 * Provides a generic tree traversal function that calls a user callback
 * for each leaf item found.
 */

#include <endian.h>
#include <fcntl.h> /* posix_fadvise */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_structures.h"
#include "btrfs/checksum.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"

/*
 * Callback type for B-tree leaf item processing.
 * Called for each item in each leaf node.
 *
 * Parameters:
 *   key       - the item's key
 *   data      - pointer to the item's data
 *   data_size - size of the item's data
 *   ctx       - user context
 *
 * Return 0 to continue iteration, non-zero to stop.
 */
typedef int (*btree_callback)(const struct btrfs_disk_key *key,
                              const void *data, uint32_t data_size, void *ctx);

/*
 * Walk a btrfs B-tree, calling the callback for each leaf item.
 *
 * Parameters:
 *   dev            - device handle
 *   chunk_map      - for logical→physical resolution
 *   root_logical   - logical address of the tree root node
 *   root_level     - level of the root node
 *   nodesize       - size of each node in bytes
 *   csum_type      - checksum algorithm type (from superblock)
 *   callback       - function to call for each leaf item
 *   ctx            - user context passed to callback
 *
 * Returns 0 on success, -1 on error.
 */
int btree_walk(struct device *dev, const struct chunk_map *chunk_map,
               uint64_t root_logical, uint8_t root_level, uint32_t nodesize,
               uint16_t csum_type, btree_callback callback, void *ctx) {
  /* Iterative DFS using explicit stack */
  struct stack_entry {
    uint64_t logical;
    uint8_t level;
  };

  /* Max stack depth = BTRFS_MAX_LEVEL * max_keys_per_node.
   * A node can contain at most (nodesize - header_size) / key_ptr_size items.
   * For 16K nodes: (16384 - 101) / 33 ≈ 493 key ptrs per level.
   * 8 levels * 493 = ~3944 entries max. Use 8192 for safety. */
  struct stack_entry *stack = malloc(8192 * sizeof(struct stack_entry));
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

    /* Resolve logical → physical */
    uint64_t node_physical = chunk_map_resolve(chunk_map, node_logical);
    if (node_physical == (uint64_t)-1) {
      fprintf(stderr,
              "btrfs2ext4: cannot resolve btree node at logical 0x%lx\n",
              (unsigned long)node_logical);
      ret = -1;
      break;
    }

    /* Read the node */
    if (device_read(dev, node_physical, node_buf, nodesize) < 0) {
      ret = -1;
      break;
    }

    const struct btrfs_header *hdr = (const struct btrfs_header *)node_buf;
    uint32_t nritems = le32toh(hdr->nritems);
    uint8_t level = hdr->level;

    /* Validate header */
    /* Check node checksum using proper btrfs logic */
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

    /* Validate header */
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

    if (level > 0) {
      /* Internal node: push children (in reverse order for DFS) */
      const struct btrfs_key_ptr *ptrs =
          (const struct btrfs_key_ptr *)(node_buf +
                                         sizeof(struct btrfs_header));

      /* Issue readahead hints for all children before descending (#12).
       * The kernel will start prefetching these nodes in parallel. */
      for (uint32_t i = 0; i < nritems; i++) {
        uint64_t child_logical = le64toh(ptrs[i].blockptr);
        uint64_t child_physical = chunk_map_resolve(chunk_map, child_logical);
        if (child_physical != (uint64_t)-1) {
          posix_fadvise(dev->fd, (off_t)child_physical, nodesize,
                        POSIX_FADV_WILLNEED);
        }
      }

      for (int i = (int)nritems - 1; i >= 0; i--) {
        if (stack_top >= 8192) {
          fprintf(stderr, "btrfs2ext4: btree walk stack overflow\n");
          ret = -1;
          goto done;
        }
        stack[stack_top].logical = le64toh(ptrs[i].blockptr);
        stack[stack_top].level = level - 1;
        stack_top++;
      }
    } else {
      /* Leaf node: process items */
      const struct btrfs_item *items =
          (const struct btrfs_item *)(node_buf + sizeof(struct btrfs_header));

      for (uint32_t i = 0; i < nritems; i++) {
        uint32_t data_offset = le32toh(items[i].offset);
        uint32_t data_size = le32toh(items[i].size);

        /* Data is stored at end of leaf, offset from byte 0x65 (header size) */
        const void *data = node_buf + sizeof(struct btrfs_header) + data_offset;

        /* Safety check */
        if ((uint64_t)sizeof(struct btrfs_header) + data_offset + data_size >
            nodesize) {
          fprintf(stderr,
                  "btrfs2ext4: btree item data out of bounds in node 0x%lx\n",
                  (unsigned long)node_logical);
          continue; /* Skip malformed item */
        }

        int cb_ret = callback(&items[i].key, data, data_size, ctx);
        if (cb_ret != 0) {
          /* Callback requested stop (not an error) */
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
