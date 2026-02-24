/*
 * fs_tree.c — Btrfs filesystem tree reader
 *
 * Reads the FS tree (tree 5) to build an in-memory representation
 * of all files, directories, and their extents.
 */

#include <endian.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/btrfs_structures.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static struct file_entry *file_entry_create(uint64_t ino) {
  struct file_entry *fe = calloc(1, sizeof(struct file_entry));
  if (!fe)
    return NULL;
  fe->ino = ino;
  fe->extent_capacity = BTRFS_INITIAL_EXTENTS_CAPACITY;
  fe->extents = calloc(fe->extent_capacity, sizeof(struct file_extent));
  if (!fe->extents) {
    free(fe);
    return NULL;
  }
  return fe;
}

static int file_entry_add_extent(struct file_entry *fe,
                                 const struct file_extent *ext) {
  if (fe->extent_count >= fe->extent_capacity) {
    uint32_t new_cap = fe->extent_capacity * 2;
    struct file_extent *new_ext =
        realloc(fe->extents, new_cap * sizeof(struct file_extent));
    if (!new_ext)
      return -1;
    fe->extents = new_ext;
    fe->extent_capacity = new_cap;
  }
  fe->extents[fe->extent_count++] = *ext;
  return 0;
}

static int file_entry_add_child(struct file_entry *parent,
                                struct file_entry *child, const char *name,
                                uint16_t name_len) {
  if (parent->child_count >= parent->child_capacity) {
    uint32_t new_cap = parent->child_capacity ? parent->child_capacity * 2 : 16;
    struct dir_entry_link *new_children =
        realloc(parent->children, new_cap * sizeof(struct dir_entry_link));
    if (!new_children)
      return -1;
    parent->children = new_children;
    parent->child_capacity = new_cap;
  }
  struct dir_entry_link *link = &parent->children[parent->child_count++];
  link->target = child;
  link->name_len =
      name_len > BTRFS_MAX_NAME_LEN ? BTRFS_MAX_NAME_LEN : name_len;
  memcpy(link->name, name, link->name_len);
  link->name[link->name_len] = '\0';
  return 0;
}

/* ========================================================================
 * Optional inode hash table accelerator
 * ======================================================================== */

static int ino_ht_grow(struct btrfs_fs_info *fs_info, uint32_t new_cap) {
  struct file_entry **new_buckets =
      calloc(new_cap, sizeof(struct file_entry *));
  if (!new_buckets)
    return -1;

  if (fs_info->ino_ht.buckets) {
    for (uint32_t i = 0; i < fs_info->ino_ht.capacity; i++) {
      struct file_entry *fe = fs_info->ino_ht.buckets[i];
      if (!fe)
        continue;
      uint32_t idx = (uint32_t)(fe->ino * 2654435761ULL) % new_cap;
      while (new_buckets[idx]) {
        idx = (idx + 1) % new_cap;
      }
      new_buckets[idx] = fe;
    }
    free(fs_info->ino_ht.buckets);
  }

  fs_info->ino_ht.buckets = new_buckets;
  fs_info->ino_ht.capacity = new_cap;
  return 0;
}

static int ino_ht_insert(struct btrfs_fs_info *fs_info, struct file_entry *fe) {
  /* Lazy init and simple load factor control (<= 50% for low collision rate) */
  if (!fs_info->ino_ht.buckets ||
      (fs_info->ino_ht.count * 2 >= fs_info->ino_ht.capacity)) {
    uint32_t new_cap =
        fs_info->ino_ht.capacity ? fs_info->ino_ht.capacity * 2 : 256;
    if (ino_ht_grow(fs_info, new_cap) < 0)
      return -1;
  }

  uint32_t cap = fs_info->ino_ht.capacity;
  uint32_t idx = (uint32_t)(fe->ino * 2654435761ULL) % cap;

  while (fs_info->ino_ht.buckets[idx] &&
         fs_info->ino_ht.buckets[idx]->ino != fe->ino) {
    idx = (idx + 1) % cap;
  }

  if (!fs_info->ino_ht.buckets[idx])
    fs_info->ino_ht.count++;

  fs_info->ino_ht.buckets[idx] = fe;
  return 0;
}

static struct file_entry *ino_ht_get(struct btrfs_fs_info *fs_info,
                                     uint64_t ino) {
  if (!fs_info->ino_ht.buckets || fs_info->ino_ht.capacity == 0)
    return NULL;

  uint32_t cap = fs_info->ino_ht.capacity;
  uint32_t idx = (uint32_t)(ino * 2654435761ULL) % cap;
  uint32_t start = idx;

  do {
    struct file_entry *fe = fs_info->ino_ht.buckets[idx];
    if (!fe)
      return NULL;
    if (fe->ino == ino)
      return fe;
    idx = (idx + 1) % cap;
  } while (idx != start);

  return NULL;
}

/* ========================================================================
 * Inode table management
 * ======================================================================== */

static int fs_info_add_inode(struct btrfs_fs_info *fs_info,
                             struct file_entry *fe) {
  if (fs_info->inode_count >= fs_info->inode_capacity) {
    uint32_t new_cap =
        fs_info->inode_capacity ? fs_info->inode_capacity * 2 : 256;
    struct file_entry **new_table =
        realloc(fs_info->inode_table, new_cap * sizeof(struct file_entry *));
    if (!new_table)
      return -1;
    fs_info->inode_table = new_table;
    fs_info->inode_capacity = new_cap;
  }
  fs_info->inode_table[fs_info->inode_count++] = fe;

  /* Best-effort insertion into hash table; fall back to linear scan on OOM */
  if (ino_ht_insert(fs_info, fe) < 0)
    fprintf(stderr,
            "btrfs2ext4: warning: inode hash table disabled (OOM), falling "
            "back to linear lookups\n");

  return 0;
}

struct file_entry *btrfs_find_inode(struct btrfs_fs_info *fs_info,
                                    uint64_t ino) {
  /* Fast path: use hash table if available */
  struct file_entry *fe = ino_ht_get(fs_info, ino);
  if (fe)
    return fe;

  /* Fallback: linear scan (used during very early phases or if hash disabled)
   */
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    if (fs_info->inode_table[i]->ino == ino)
      return fs_info->inode_table[i];
  }
  return NULL;
}

static struct file_entry *find_or_create_inode(struct btrfs_fs_info *fs_info,
                                               uint64_t ino) {
  struct file_entry *fe = btrfs_find_inode(fs_info, ino);
  if (fe)
    return fe;

  fe = file_entry_create(ino);
  if (!fe)
    return NULL;
  if (fs_info_add_inode(fs_info, fe) < 0) {
    free(fe->extents);
    free(fe);
    return NULL;
  }
  return fe;
}

/* ========================================================================
 * B-tree callback for FS tree items
 * ======================================================================== */

/* Forward declaration of btree_walk callback type */
typedef int (*btree_callback)(const struct btrfs_disk_key *key,
                              const void *data, uint32_t data_size, void *ctx);
extern int btree_walk(struct device *dev, const struct chunk_map *chunk_map,
                      uint64_t root_logical, uint8_t root_level,
                      uint32_t nodesize, uint16_t csum_type,
                      btree_callback callback, void *ctx);

/* ========================================================================
 * CoW Deduplication Hash Table (Phase 4.1)
 * ======================================================================== */

struct cow_hash {
  uint64_t *buckets;
  uint32_t capacity;
  uint32_t count;
};

static void cow_hash_init(struct cow_hash *h, uint32_t initial_cap) {
  h->capacity = initial_cap;
  h->count = 0;
  h->buckets = calloc(h->capacity, sizeof(uint64_t));
}

static int cow_hash_rehash(struct cow_hash *h) {
  uint32_t old_cap = h->capacity;
  uint64_t *old_buckets = h->buckets;

  if (old_cap > UINT32_MAX / 2) {
    fprintf(stderr, "btrfs2ext4: cow_hash: cannot grow beyond 2^31 buckets\n");
    return -1;
  }

  h->capacity = old_cap * 2;
  h->buckets = calloc(h->capacity, sizeof(uint64_t));
  if (!h->buckets) {
    h->buckets = old_buckets;
    h->capacity = old_cap;
    return -1;
  }
  h->count = 0;

  for (uint32_t i = 0; i < old_cap; i++) {
    if (old_buckets[i] == 0)
      continue;
    uint64_t key = old_buckets[i];
    uint32_t idx = (uint32_t)(key * 2654435761ULL) % h->capacity;
    while (h->buckets[idx] != 0)
      idx = (idx + 1) % h->capacity;
    h->buckets[idx] = key;
    h->count++;
  }

  free(old_buckets);
  return 0;
}

static int cow_hash_check_and_add(struct cow_hash *h, uint64_t bytenr) {
  if (h->count * 2 >= h->capacity) {
    if (cow_hash_rehash(h) < 0)
      return -1;
  }

  uint32_t idx = (uint32_t)(bytenr * 2654435761ULL) % h->capacity;
  while (h->buckets[idx] != 0) {
    if (h->buckets[idx] == bytenr) {
      return 1; /* Already seen! It's a CoW duplicate */
    }
    idx = (idx + 1) % h->capacity;
  }

  h->buckets[idx] = bytenr;
  h->count++;
  return 0; /* First time seeing this physical layout */
}

struct fs_tree_ctx {
  struct btrfs_fs_info *fs_info;
  struct cow_hash cow_track;
};

static int fs_tree_callback(const struct btrfs_disk_key *key, const void *data,
                            uint32_t data_size, void *ctx) {
  struct fs_tree_ctx *fctx = (struct fs_tree_ctx *)ctx;
  struct btrfs_fs_info *fs_info = fctx->fs_info;

  uint64_t objectid = le64toh(key->objectid);
  uint8_t type = key->type;

  switch (type) {
  case BTRFS_INODE_ITEM_KEY: {
    if (data_size < sizeof(struct btrfs_inode_item))
      break;

    const struct btrfs_inode_item *ii = (const struct btrfs_inode_item *)data;
    struct file_entry *fe = find_or_create_inode(fs_info, objectid);
    if (!fe)
      return -1;

    fe->mode = le32toh(ii->mode);
    fe->uid = le32toh(ii->uid);
    fe->gid = le32toh(ii->gid);
    fe->nlink = le32toh(ii->nlink);
    fe->size = le64toh(ii->size);
    fe->rdev = le64toh(ii->rdev);

    fe->atime_sec = (int64_t)le64toh(*(uint64_t *)&ii->atime.sec);
    fe->atime_nsec = le32toh(ii->atime.nsec);
    fe->mtime_sec = (int64_t)le64toh(*(uint64_t *)&ii->mtime.sec);
    fe->mtime_nsec = le32toh(ii->mtime.nsec);
    fe->ctime_sec = (int64_t)le64toh(*(uint64_t *)&ii->ctime.sec);
    fe->ctime_nsec = le32toh(ii->ctime.nsec);
    fe->crtime_sec = (int64_t)le64toh(*(uint64_t *)&ii->otime.sec);
    fe->crtime_nsec = le32toh(ii->otime.nsec);
    break;
  }

  case BTRFS_INODE_REF_KEY: {
    if (data_size < sizeof(struct btrfs_inode_ref))
      break;

    uint64_t parent_ino = le64toh(key->offset);

    struct file_entry *fe = find_or_create_inode(fs_info, objectid);
    if (!fe)
      return -1;

    /* Set primary parent_ino for '..' directory links */
    if (fe->parent_ino == 0) {
      fe->parent_ino = parent_ino;
    }

    /* Note: Hard link names are safely ignored here,
     * they are parsed via BTRFS_DIR_INDEX_KEY into their respective directories
     */
    break;
  }

  case BTRFS_DIR_INDEX_KEY: {
    /* We use DIR_INDEX instead of DIR_ITEM to avoid hash collisions.
     * DIR_INDEX entries have a 1:1 mapping. */
    if (data_size < sizeof(struct btrfs_dir_item))
      break;

    const struct btrfs_dir_item *di = (const struct btrfs_dir_item *)data;
    uint64_t child_ino = le64toh(di->location.objectid);
    uint64_t parent_ino = objectid;
    uint16_t name_len = le16toh(di->name_len);

    /* Security check: Prevent OOB read if name_len is maliciously large */
    if (data_size < sizeof(struct btrfs_dir_item) + name_len)
      break;

    struct file_entry *parent = find_or_create_inode(fs_info, parent_ino);
    struct file_entry *child = find_or_create_inode(fs_info, child_ino);
    if (!parent || !child)
      return -1;

    const char *name = (const char *)(di + 1);
    file_entry_add_child(parent, child, name, name_len);
    break;
  }

  case BTRFS_EXTENT_DATA_KEY: {
    if (data_size < sizeof(struct btrfs_file_extent_item) - 32)
      break; /* At minimum, need the header fields */

    const struct btrfs_file_extent_item *fi =
        (const struct btrfs_file_extent_item *)data;

    struct file_entry *fe = find_or_create_inode(fs_info, objectid);
    if (!fe)
      return -1;

    struct file_extent ext;
    memset(&ext, 0, sizeof(ext));
    ext.file_offset = le64toh(key->offset);
    ext.compression = fi->compression;
    ext.type = fi->type;
    ext.ram_bytes = le64toh(fi->ram_bytes);

    if (fi->type == BTRFS_FILE_EXTENT_INLINE) {
      /* Inline data: stored directly after the fixed fields */
      size_t header_size = offsetof(struct btrfs_file_extent_item, disk_bytenr);
      if (data_size > header_size) {
        ext.inline_data_len = data_size - header_size;
        ext.inline_data = malloc(ext.inline_data_len);
        if (ext.inline_data) {
          memcpy(ext.inline_data, (const uint8_t *)data + header_size,
                 ext.inline_data_len);
        }
      }
    } else {
      /* Regular or prealloc extent */
      if (data_size >= sizeof(struct btrfs_file_extent_item)) {
        ext.disk_bytenr = le64toh(fi->disk_bytenr);
        ext.disk_num_bytes = le64toh(fi->disk_num_bytes);
        ext.num_bytes = le64toh(fi->num_bytes);
        /* fi->offset is the offset within the extent */

        /* CoW Deduplication Tracking (Phase 4.1) */
        if (ext.disk_bytenr != 0 && ext.type != BTRFS_FILE_EXTENT_INLINE) {
          if (cow_hash_check_and_add(&fctx->cow_track, ext.disk_bytenr)) {
            /* We have seen this physical block sequence before. Needs clone. */
            fs_info->shared_extent_count++;

            /* Add to required deduplication physical blocks count */
            uint32_t block_size =
                fs_info->sb.sectorsize ? le32toh(fs_info->sb.sectorsize) : 4096;
            fs_info->dedup_blocks_needed +=
                (ext.disk_num_bytes + block_size - 1) / block_size;
          }
        }
      }
    }

    file_entry_add_extent(fe, &ext);
    break;
  }

  case BTRFS_XATTR_ITEM_KEY: {
    /* Phase 6.1: Parse Extended Attributes & ACLs */
    if (data_size < sizeof(struct btrfs_dir_item))
      break;

    const struct btrfs_dir_item *di = (const struct btrfs_dir_item *)data;
    uint16_t name_len = le16toh(di->name_len);
    uint16_t data_len = le16toh(di->data_len);

    if (data_size < sizeof(struct btrfs_dir_item) + name_len + data_len)
      break; /* Bounds check */

    struct file_entry *fe = find_or_create_inode(fs_info, objectid);
    if (!fe)
      break;

    struct xattr_entry *xattr = malloc(sizeof(struct xattr_entry));
    if (xattr) {
      xattr->name_len = name_len;
      xattr->value_len = data_len;
      xattr->name = malloc(name_len + 1);
      xattr->value = data_len > 0 ? malloc(data_len) : NULL;

      if (xattr->name && (data_len == 0 || xattr->value)) {
        const uint8_t *payload = (const uint8_t *)(di + 1);
        memcpy(xattr->name, payload, name_len);
        xattr->name[name_len] = '\0';
        if (data_len > 0)
          memcpy(xattr->value, payload + name_len, data_len);

        /* Link it */
        xattr->next = fe->xattrs;
        fe->xattrs = xattr;
      } else {
        free(xattr->name);
        free(xattr->value);
        free(xattr);
      }
    }
    break;
  }

  default:
    /* Ignore other item types */
    break;
  }

  return 0;
}

/* ========================================================================
 * B-tree callback for root tree (to find FS tree root)
 * ======================================================================== */

struct root_tree_ctx {
  uint64_t fs_tree_bytenr;
  uint8_t fs_tree_level;
  int found;
  uint64_t extent_tree_bytenr;
  uint8_t extent_tree_level;
  int found_extent;
};

static int root_tree_callback(const struct btrfs_disk_key *key,
                              const void *data, uint32_t data_size, void *ctx) {
  struct root_tree_ctx *rctx = (struct root_tree_ctx *)ctx;

  if (key->type != BTRFS_ROOT_ITEM_KEY)
    return 0;

  uint64_t objectid = le64toh(key->objectid);

  if (objectid == BTRFS_FS_TREE_OBJECTID) {
    if (data_size >= sizeof(struct btrfs_root_item)) {
      const struct btrfs_root_item *ri = (const struct btrfs_root_item *)data;
      rctx->fs_tree_bytenr = le64toh(ri->bytenr);
      rctx->fs_tree_level = ri->level;
      rctx->found = 1;
      printf("Found FS tree root: bytenr=0x%lx level=%u\n",
             (unsigned long)rctx->fs_tree_bytenr, rctx->fs_tree_level);
    }
    return 0; /* Continue to find all trees */
  } else if (objectid == BTRFS_EXTENT_TREE_OBJECTID) {
    if (data_size >= sizeof(struct btrfs_root_item)) {
      const struct btrfs_root_item *ri = (const struct btrfs_root_item *)data;
      rctx->extent_tree_bytenr = le64toh(ri->bytenr);
      rctx->extent_tree_level = ri->level;
      rctx->found_extent = 1;
      printf("Found Extent tree root: bytenr=0x%lx level=%u\n",
             (unsigned long)rctx->extent_tree_bytenr, rctx->extent_tree_level);
    }
    return 0;
  }

  return 0;
}

/* ========================================================================
 * B-tree callback for extent tree (to build used-block map)
 * ======================================================================== */

struct extent_tree_ctx {
  struct used_block_map *map;
};

static int used_block_map_add(struct used_block_map *map, uint64_t start,
                              uint64_t length, uint64_t flags) {
  if (map->count >= map->capacity) {
    uint32_t new_cap = map->capacity ? map->capacity * 2 : 256;
    struct used_extent *new_ext =
        realloc(map->extents, new_cap * sizeof(struct used_extent));
    if (!new_ext)
      return -1;
    map->extents = new_ext;
    map->capacity = new_cap;
  }
  map->extents[map->count].start = start;
  map->extents[map->count].length = length;
  map->extents[map->count].flags = flags;
  map->count++;
  return 0;
}

static int extent_tree_callback(const struct btrfs_disk_key *key,
                                const void *data, uint32_t data_size,
                                void *ctx) {
  struct extent_tree_ctx *ectx = (struct extent_tree_ctx *)ctx;

  if (key->type == BTRFS_EXTENT_ITEM_KEY ||
      key->type == BTRFS_METADATA_ITEM_KEY) {
    if (data_size >= sizeof(struct btrfs_extent_item)) {
      const struct btrfs_extent_item *ei =
          (const struct btrfs_extent_item *)data;
      uint64_t start = le64toh(key->objectid);
      uint64_t length;

      if (key->type == BTRFS_EXTENT_ITEM_KEY) {
        length = le64toh(key->offset);
      } else {
        /* METADATA_ITEM uses nodesize as implicit length */
        length = 0; /* Will be filled in later with nodesize */
      }

      uint64_t flags = le64toh(ei->flags);
      used_block_map_add(ectx->map, start, length, flags);
    }
  }

  return 0;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

extern int btrfs_read_superblock(struct device *dev,
                                 struct btrfs_super_block *sb);

int btrfs_read_fs(struct device *dev, struct btrfs_fs_info *fs_info) {
  memset(fs_info, 0, sizeof(*fs_info));
  fs_info->dev = dev;

  printf("=== Phase 1: Reading Btrfs Metadata ===\n\n");

  /* Step 1: Read superblock */
  printf("Step 1/6: Reading superblock...\n");
  if (btrfs_read_superblock(dev, &fs_info->sb) < 0)
    return -1;

  /* Step 2: Bootstrap chunk mappings from sys_chunk_array */
  printf("Step 2/6: Bootstrapping chunk mappings...\n");
  fs_info->chunk_map = calloc(1, sizeof(struct chunk_map));
  if (!fs_info->chunk_map)
    return -1;

  if (chunk_map_init_from_superblock(fs_info->chunk_map, &fs_info->sb) < 0)
    return -1;

  /* Step 3: Fully populate chunk map */
  printf("Step 3/6: Walking chunk tree...\n");
  if (chunk_map_populate(fs_info->chunk_map, dev, &fs_info->sb) < 0)
    return -1;

  /* Step 4: Walk root tree to find FS tree and extent tree roots */
  printf("Step 4/6: Walking root tree...\n");
  uint64_t root_tree_logical = le64toh(fs_info->sb.root);
  uint8_t root_tree_level = fs_info->sb.root_level;
  uint32_t nodesize = le32toh(fs_info->sb.nodesize);

  struct root_tree_ctx rctx;
  memset(&rctx, 0, sizeof(rctx));

  if (btree_walk(dev, fs_info->chunk_map, root_tree_logical, root_tree_level,
                 nodesize, le16toh(fs_info->sb.csum_type), root_tree_callback,
                 &rctx) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to walk root tree\n");
    return -1;
  }

  if (!rctx.found) {
    fprintf(stderr, "btrfs2ext4: FS tree (tree 5) not found in root tree\n");
    return -1;
  }

  /* Step 5: Walk FS tree to build file/directory tree */
  printf("Step 5/6: Walking filesystem tree...\n");
  struct fs_tree_ctx fctx;
  memset(&fctx, 0, sizeof(fctx));
  fctx.fs_info = fs_info;
  cow_hash_init(&fctx.cow_track, 1024);

  if (btree_walk(dev, fs_info->chunk_map, rctx.fs_tree_bytenr,
                 rctx.fs_tree_level, nodesize, le16toh(fs_info->sb.csum_type),
                 fs_tree_callback, &fctx) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to walk FS tree\n");
    free(fctx.cow_track.buckets);
    return -1;
  }

  free(fctx.cow_track.buckets);

  /* Step 6: Walk extent tree to build used-block map */
  printf("Step 6/6: Walking extent tree...\n");

  /* The extent tree is rooted separately, we need to find it in the root tree.
   */
  struct extent_tree_ctx ectx;
  ectx.map = &fs_info->used_blocks;

  if (rctx.found_extent) {
    if (btree_walk(dev, fs_info->chunk_map, rctx.extent_tree_bytenr,
                   rctx.extent_tree_level, nodesize,
                   le16toh(fs_info->sb.csum_type), extent_tree_callback,
                   &ectx) < 0) {
      fprintf(stderr, "btrfs2ext4: warning: extent tree walk failed, "
                      "using FS tree extents only\n");
      rctx.found_extent = 0;
    }
  }

  if (!rctx.found_extent) {
    /* Build used-block map from FS tree data extents (sufficient for v1) */
    for (uint32_t i = 0; i < fs_info->inode_count; i++) {
      const struct file_entry *fe = fs_info->inode_table[i];
      for (uint32_t j = 0; j < fe->extent_count; j++) {
        const struct file_extent *ext = &fe->extents[j];
        if (ext->type == BTRFS_FILE_EXTENT_INLINE || ext->disk_bytenr == 0)
          continue;
        used_block_map_add(&fs_info->used_blocks, ext->disk_bytenr,
                           ext->disk_num_bytes, BTRFS_BLOCK_GROUP_DATA);
      }
    }
  }
  printf("  Built used-block map: %u extents\n", fs_info->used_blocks.count);

  /* Compute compression statistics for space check in Pass 2 */
  fs_info->total_compressed_bytes = 0;
  fs_info->total_decompressed_bytes = 0;
  fs_info->compressed_extent_count = 0;

  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    const struct file_entry *fe = fs_info->inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      const struct file_extent *ext = &fe->extents[j];
      if (ext->compression != BTRFS_COMPRESS_NONE &&
          ext->type != BTRFS_FILE_EXTENT_INLINE && ext->disk_bytenr != 0) {
        fs_info->total_compressed_bytes += ext->disk_num_bytes;
        fs_info->total_decompressed_bytes += ext->ram_bytes;
        fs_info->compressed_extent_count++;
      }
    }
  }

  if (fs_info->compressed_extent_count > 0) {
    printf("  Compressed extents:  %u\n", fs_info->compressed_extent_count);
    printf("  Compressed size:     %lu bytes (%.1f MiB)\n",
           (unsigned long)fs_info->total_compressed_bytes,
           (double)fs_info->total_compressed_bytes / (1024.0 * 1024.0));
    printf("  Decompressed size:   %lu bytes (%.1f MiB)\n",
           (unsigned long)fs_info->total_decompressed_bytes,
           (double)fs_info->total_decompressed_bytes / (1024.0 * 1024.0));
    printf("  Expansion needed:    %.1f MiB\n",
           (double)(fs_info->total_decompressed_bytes -
                    fs_info->total_compressed_bytes) /
               (1024.0 * 1024.0));
  }

  /* Suppress unused warning — extent_tree_callback will be used for full
   * extent tree walking in a future version */
  (void)extent_tree_callback;

  /* Read symlink targets for symlink inodes */
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    struct file_entry *fe = fs_info->inode_table[i];
    if (!S_ISLNK(fe->mode))
      continue;

    /* Symlink target is stored as inline extent data */
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      if (fe->extents[j].type == BTRFS_FILE_EXTENT_INLINE &&
          fe->extents[j].inline_data != NULL) {
        size_t tlen = fe->extents[j].inline_data_len;
        if (tlen == 0 || tlen > PATH_MAX) {
          fprintf(stderr,
                  "btrfs2ext4: symlink ino %lu has suspicious target len %zu\n",
                  (unsigned long)fe->ino, tlen);
          break;
        }
        fe->symlink_target = malloc(tlen + 1);
        if (fe->symlink_target) {
          memcpy(fe->symlink_target, fe->extents[j].inline_data, tlen);
          fe->symlink_target[tlen] = '\0';
        }
        break;
      }
    }
  }

  /* Find root directory (inode 256 is the default subvolume root) */
  fs_info->root_dir = btrfs_find_inode(fs_info, BTRFS_FIRST_FREE_OBJECTID);
  if (!fs_info->root_dir) {
    fprintf(stderr, "btrfs2ext4: root directory (inode 256) not found\n");
    return -1;
  }

  printf("\n=== Btrfs Metadata Summary ===\n");
  printf("  Total inodes read: %u\n", fs_info->inode_count);
  printf("  Used extents:      %u\n", fs_info->used_blocks.count);
  printf("  Root directory:    inode %lu\n",
         (unsigned long)fs_info->root_dir->ino);
  printf("==============================\n\n");

  return 0;
}

void btrfs_free_fs(struct btrfs_fs_info *fs_info) {
  /* Free all file entries */
  if (fs_info->inode_table) {
    for (uint32_t i = 0; i < fs_info->inode_count; i++) {
      struct file_entry *fe = fs_info->inode_table[i];
      if (fe) {
        /* Free inline data */
        for (uint32_t j = 0; j < fe->extent_count; j++) {
          free(fe->extents[j].inline_data);
        }

        /* Free xattrs */
        struct xattr_entry *xa = fe->xattrs;
        while (xa) {
          struct xattr_entry *next = xa->next;
          free(xa->name);
          free(xa->value);
          free(xa);
          xa = next;
        }

        free(fe->extents);
        free(fe->children);
        free(fe->symlink_target);
        free(fe);
      }
    }
    free(fs_info->inode_table);
  }

  /* Free chunk map */
  if (fs_info->chunk_map) {
    chunk_map_free(fs_info->chunk_map);
    free(fs_info->chunk_map);
  }

  /* Free used block map */
  free(fs_info->used_blocks.extents);

  /* Free inode hash table */
  free(fs_info->ino_ht.buckets);
  fs_info->ino_ht.buckets = NULL;
  fs_info->ino_ht.capacity = 0;
  fs_info->ino_ht.count = 0;

  memset(fs_info, 0, sizeof(*fs_info));
}
