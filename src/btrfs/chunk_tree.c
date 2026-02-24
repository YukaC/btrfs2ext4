/*
 * chunk_tree.c — Btrfs chunk tree resolver
 *
 * Implements logical-to-physical address resolution by parsing the
 * sys_chunk_array and then walking the chunk tree.
 */

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btrfs/btrfs_structures.h"
#include "btrfs/checksum.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"

#define INITIAL_CHUNK_CAPACITY 64

static int chunk_map_add(struct chunk_map *map, uint64_t logical,
                         uint64_t physical, uint64_t length, uint64_t type) {
  /* Check for duplicates */
  for (uint32_t i = 0; i < map->count; i++) {
    if (map->entries[i].logical == logical)
      return 0; /* Already exists */
  }

  if (map->count >= map->capacity) {
    uint32_t new_cap = map->capacity * 2;
    struct chunk_mapping *new_entries =
        realloc(map->entries, new_cap * sizeof(struct chunk_mapping));
    if (!new_entries) {
      fprintf(stderr, "btrfs2ext4: out of memory for chunk map\n");
      return -1;
    }
    map->entries = new_entries;
    map->capacity = new_cap;
  }

  struct chunk_mapping *e = &map->entries[map->count++];
  e->logical = logical;
  e->physical = physical;
  e->length = length;
  e->type = type;

  return 0;
}

/* Compare function for sorting chunk map by logical address */
static int chunk_cmp(const void *a, const void *b) {
  const struct chunk_mapping *ca = (const struct chunk_mapping *)a;
  const struct chunk_mapping *cb = (const struct chunk_mapping *)b;
  if (ca->logical < cb->logical)
    return -1;
  if (ca->logical > cb->logical)
    return 1;
  return 0;
}

int chunk_map_init_from_superblock(struct chunk_map *map,
                                   const struct btrfs_super_block *sb) {
  memset(map, 0, sizeof(*map));
  map->capacity = INITIAL_CHUNK_CAPACITY;
  map->entries = calloc(map->capacity, sizeof(struct chunk_mapping));
  if (!map->entries) {
    fprintf(stderr, "btrfs2ext4: out of memory for chunk map\n");
    return -1;
  }

  /* Parse sys_chunk_array from superblock */
  uint32_t array_size = le32toh(sb->sys_chunk_array_size);
  if (array_size == 0 || array_size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE) {
    fprintf(stderr,
            "btrfs2ext4: invalid sys_chunk_array_size=%u "
            "(max=%u) — superblock corrupt or unsupported\n",
            array_size, BTRFS_SYSTEM_CHUNK_ARRAY_SIZE);
    free(map->entries);
    map->entries = NULL;
    map->capacity = 0;
    return -1;
  }
  const uint8_t *p = sb->sys_chunk_array;
  const uint8_t *end = p + array_size;

  printf("Parsing sys_chunk_array (%u bytes)...\n", array_size);

  while (p < end) {
    /* Each entry is: btrfs_disk_key + btrfs_chunk + stripes */
    if (p + sizeof(struct btrfs_disk_key) > end) {
      fprintf(stderr, "btrfs2ext4: truncated sys_chunk_array (key)\n");
      return -1;
    }

    const struct btrfs_disk_key *key = (const struct btrfs_disk_key *)p;
    p += sizeof(struct btrfs_disk_key);

    if (key->type != BTRFS_CHUNK_ITEM_KEY) {
      fprintf(stderr,
              "btrfs2ext4: unexpected key type 0x%02x in sys_chunk_array\n",
              key->type);
      return -1;
    }

    if (p + sizeof(struct btrfs_chunk) > end) {
      fprintf(stderr, "btrfs2ext4: truncated sys_chunk_array (chunk)\n");
      return -1;
    }

    const struct btrfs_chunk *chunk = (const struct btrfs_chunk *)p;
    uint16_t num_stripes = le16toh(chunk->num_stripes);
    size_t chunk_size =
        sizeof(struct btrfs_chunk) + num_stripes * sizeof(struct btrfs_stripe);

    if (p + chunk_size > end) {
      fprintf(stderr, "btrfs2ext4: truncated sys_chunk_array (stripes)\n");
      return -1;
    }

    /* For single-device, use stripe[0] */
    const struct btrfs_stripe *stripe =
        (const struct btrfs_stripe *)(p + sizeof(struct btrfs_chunk));

    uint64_t logical = le64toh(key->offset);
    uint64_t physical = le64toh(stripe->offset);
    uint64_t length = le64toh(chunk->length);
    uint64_t type = le64toh(chunk->type);

    printf("  Chunk: logical=0x%lx physical=0x%lx length=0x%lx type=0x%lx\n",
           (unsigned long)logical, (unsigned long)physical,
           (unsigned long)length, (unsigned long)type);

    if (chunk_map_add(map, logical, physical, length, type) < 0)
      return -1;

    p += chunk_size;
  }

  /* Sort by logical address for binary search */
  qsort(map->entries, map->count, sizeof(struct chunk_mapping), chunk_cmp);

  printf("  Parsed %u system chunks\n\n", map->count);
  return 0;
}

int chunk_map_populate(struct chunk_map *map, struct device *dev,
                       const struct btrfs_super_block *sb) {
  /*
   * Walk the chunk tree to get ALL chunk mappings (not just system chunks).
   * The chunk tree root is at sb->chunk_root (logical address).
   * We can resolve it using the bootstrap mappings already loaded.
   */

  uint64_t chunk_root_logical = le64toh(sb->chunk_root);
  uint8_t chunk_root_level = sb->chunk_root_level;
  uint32_t nodesize = le32toh(sb->nodesize);
  uint16_t csum_type = le16toh(sb->csum_type);

  printf("Walking chunk tree (root=0x%lx, level=%u, nodesize=%u)...\n",
         (unsigned long)chunk_root_logical, chunk_root_level, nodesize);

  /* Allocate buffer for reading tree nodes */
  uint8_t *node_buf = malloc(nodesize);
  if (!node_buf) {
    fprintf(stderr, "btrfs2ext4: out of memory for node buffer\n");
    return -1;
  }

  /* Recursive tree walk - use a simple stack */
  struct {
    uint64_t logical;
    uint8_t level;
  } stack[BTRFS_MAX_LEVEL * 256]; /* generous stack */
  int stack_top = 0;

  stack[stack_top].logical = chunk_root_logical;
  stack[stack_top].level = chunk_root_level;
  stack_top++;

  while (stack_top > 0) {
    stack_top--;
    uint64_t node_logical = stack[stack_top].logical;

    /* Resolve logical → physical */
    uint64_t node_physical = chunk_map_resolve(map, node_logical);
    if (node_physical == (uint64_t)-1) {
      fprintf(stderr,
              "btrfs2ext4: cannot resolve chunk tree node at logical 0x%lx\n",
              (unsigned long)node_logical);
      free(node_buf);
      return -1;
    }

    /* Read the node */
    if (device_read(dev, node_physical, node_buf, nodesize) < 0) {
      free(node_buf);
      return -1;
    }

    const struct btrfs_header *hdr = (const struct btrfs_header *)node_buf;
    uint32_t nritems = le32toh(hdr->nritems);
    uint8_t level = hdr->level;

    /* Validate checksum for chunk tree nodes as well */
    if (btrfs_verify_checksum(csum_type, hdr->csum,
                              (const uint8_t *)hdr + BTRFS_CSUM_SIZE,
                              nodesize - BTRFS_CSUM_SIZE) != 0) {
      fprintf(stderr,
              "btrfs2ext4: chunk tree node checksum mismatch at logical 0x%lx "
              "(algorithm: %s)\n",
              (unsigned long)node_logical, btrfs_csum_name(csum_type));
      free(node_buf);
      return -1;
    }

    uint32_t max_items =
        (nodesize - sizeof(struct btrfs_header)) / sizeof(struct btrfs_key_ptr);
    if (nritems > max_items) {
      fprintf(stderr,
              "btrfs2ext4: chunk tree node nritems=%u exceeds "
              "theoretical max=%u — corrupt node\n",
              nritems, max_items);
      free(node_buf);
      return -1;
    }

    if (level > 0) {
      /* Internal node: push children onto stack */
      const struct btrfs_key_ptr *ptrs =
          (const struct btrfs_key_ptr *)(node_buf +
                                         sizeof(struct btrfs_header));

      for (uint32_t i = 0; i < nritems; i++) {
        if (stack_top >= (int)(sizeof(stack) / sizeof(stack[0]))) {
          fprintf(stderr, "btrfs2ext4: chunk tree walk stack overflow\n");
          free(node_buf);
          return -1;
        }
        stack[stack_top].logical = le64toh(ptrs[i].blockptr);
        stack[stack_top].level = level - 1;
        stack_top++;
      }
    } else {
      /* Leaf node: extract chunk items */
      const struct btrfs_item *items =
          (const struct btrfs_item *)(node_buf + sizeof(struct btrfs_header));

      for (uint32_t i = 0; i < nritems; i++) {
        if (items[i].key.type != BTRFS_CHUNK_ITEM_KEY)
          continue;

        uint32_t data_offset = le32toh(items[i].offset);
        uint32_t data_size = le32toh(items[i].size);

        if ((uint64_t)sizeof(struct btrfs_header) + data_offset + data_size >
            (uint64_t)nodesize) {
          fprintf(stderr, "btrfs2ext4: chunk tree item OOB\n");
          continue;
        }

        if (data_size <
            sizeof(struct btrfs_chunk) + sizeof(struct btrfs_stripe)) {
          fprintf(stderr, "btrfs2ext4: chunk item too small\n");
          continue;
        }

        const struct btrfs_chunk *chunk =
            (const struct btrfs_chunk *)(node_buf +
                                         sizeof(struct btrfs_header) +
                                         data_offset);

        uint16_t num_stripes = le16toh(chunk->num_stripes);
        size_t expected_size =
            sizeof(struct btrfs_chunk) +
            (size_t)num_stripes * sizeof(struct btrfs_stripe);
        if (expected_size > data_size) {
          fprintf(stderr,
                  "btrfs2ext4: chunk item stripe count exceeds item size\n");
          continue;
        }

        const struct btrfs_stripe *stripe =
            (const struct btrfs_stripe *)((const uint8_t *)chunk +
                                          sizeof(struct btrfs_chunk));

        uint64_t logical = le64toh(items[i].key.offset);
        uint64_t physical = le64toh(stripe->offset);
        uint64_t length = le64toh(chunk->length);
        uint64_t type = le64toh(chunk->type);

        if (chunk_map_add(map, logical, physical, length, type) < 0) {
          free(node_buf);
          return -1;
        }
      }
    }
  }

  free(node_buf);

  /* Re-sort after adding new entries */
  qsort(map->entries, map->count, sizeof(struct chunk_mapping), chunk_cmp);

  printf("  Total chunk mappings: %u\n\n", map->count);
  return 0;
}

uint64_t chunk_map_resolve(const struct chunk_map *map, uint64_t logical) {
  /* Binary search for the chunk containing this logical address */
  int lo = 0, hi = (int)map->count - 1;

  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    const struct chunk_mapping *e = &map->entries[mid];

    if (logical < e->logical) {
      hi = mid - 1;
    } else if (logical >= e->logical + e->length) {
      lo = mid + 1;
    } else {
      /* Found: logical is within [e->logical, e->logical + e->length) */
      return e->physical + (logical - e->logical);
    }
  }

  return (uint64_t)-1; /* Not found */
}

void chunk_map_free(struct chunk_map *map) {
  free(map->entries);
  map->entries = NULL;
  map->count = 0;
  map->capacity = 0;
}
