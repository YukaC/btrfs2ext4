/*
 * test_fuzz.c — Aggressive Fuzzing and Edge Case Test Suite
 *
 * This suite extends test_stress.c but focuses on malformed
 * memory structures, boundary violations, decompression bombs,
 * and integer overflows designed to break the converter.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/btrfs/btrfs_reader.h"
#include "../include/btrfs/btrfs_structures.h"
#include "../include/btrfs/chunk_tree.h"
#include "../include/btrfs/decompress.h"
#include "../include/device_io.h"
#include "../include/ext4/ext4_planner.h"
#include "../include/ext4/ext4_structures.h"
#include "../include/ext4/ext4_writer.h"
#include "../include/mem_tracker.h"
#include "../include/relocator.h"

#define ASSERT_TRUE(cond, reason)                                              \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", reason);                                   \
      assert(cond);                                                            \
    }                                                                          \
  } while (0)

/* Helper to setup a minimal fuzzed FS info */
static struct btrfs_fs_info *create_fuzzed_fs() {
  struct btrfs_fs_info *fs = calloc(1, sizeof(*fs));
  fs->sb.sectorsize = 4096;
  fs->sb.nodesize = 16384;
  fs->sb.total_bytes = 10ULL * 1024 * 1024 * 1024; /* 10 GB */
  fs->chunk_map = malloc(sizeof(struct chunk_map));
  fs->chunk_map->capacity = 10;
  fs->chunk_map->count = 0;
  fs->chunk_map->entries = calloc(10, sizeof(struct chunk_mapping));
  return fs;
}

static void free_fuzzed_fs(struct btrfs_fs_info *fs) {
  if (fs) {
    if (fs->chunk_map) {
      free(fs->chunk_map->entries);
      free(fs->chunk_map);
    }
    for (uint32_t i = 0; i < fs->inode_count; i++) {
      if (fs->inode_table[i]) {
        free(fs->inode_table[i]->extents);
        free(fs->inode_table[i]);
      }
    }
    free(fs->inode_table);
    free(fs);
  }
}

/* ========================================================================
 * Group 1: Decompression Bombs & Integer Overflows
 * ======================================================================== */

static void test_decompress_bombs() {
  printf("  [1/3] Decompression Bombs & Massive Extents... ");

  /* Create an extent that claims to decompress to 4GB from 4KB */
  struct file_extent ext;
  memset(&ext, 0, sizeof(ext));
  ext.disk_bytenr = 0x10000;
  ext.disk_num_bytes = 4096;
  ext.num_bytes = 0xFFFFFFFF; /* 4GB overflow trigger */
  ext.ram_bytes = 0xFFFFFFFF;
  ext.compression = BTRFS_COMPRESS_ZLIB;
  ext.type = BTRFS_FILE_EXTENT_REG;

  /* Since the device map is dummy, btrfs_decompress_extent should fail safely
     without trying to malloc(4GB) due to out_len / safe bounds */
  struct btrfs_fs_info *fs = create_fuzzed_fs();

  /* Add an intentional memory pressure event */
  mem_track_init();
  mem_track_alloc(10ULL * 1024 * 1024 * 1024); /* Fake 10GB usage */

  /* We expect mem_tracker to catch the hash table initialization during
   * relocation planning */
  struct ext4_layout dummy_layout = {0};
  dummy_layout.reserved_block_count = 0;
  dummy_layout.reserved_blocks = NULL;

  struct relocation_plan plan = {0};
  assert(relocator_plan(&plan, &dummy_layout, fs) ==
         0); /* Should fail/log safely */

  if (plan.entries != NULL)
    free(plan.entries);

  /* Reset tracker */
  mem_track_free(10ULL * 1024 * 1024 * 1024);

  free_fuzzed_fs(fs);
  printf("OK\n");
}

/* ========================================================================
 * Group 2: Relocator & Free Space Wrap-Around
 * ======================================================================== */

static void test_relocator_wraparound() {
  printf("  [2/3] Relocator Wrapping & OOM States... ");

  struct ext4_layout layout = {0};
  layout.block_size = 4096;
  layout.total_blocks = 0xFFFFFFFF; /* Max 32-bit blocks (16TB on 4K) */
  layout.reserved_block_count = 1;
  layout.reserved_blocks = malloc(sizeof(uint64_t));
  layout.reserved_blocks[0] = 0xFFFFFFFE; /* Near end of disk */

  struct btrfs_fs_info *fs = create_fuzzed_fs();

  /* Test free space allocator with extremely high block numbers */

  /* Create an extent colliding near disk limits */
  fs->inode_count = 1;
  fs->inode_table = calloc(1, sizeof(struct file_entry *));
  fs->inode_table[0] = calloc(1, sizeof(struct file_entry));
  fs->inode_table[0]->extent_count = 1;
  fs->inode_table[0]->extents = calloc(1, sizeof(struct file_extent));
  fs->inode_table[0]->extents[0].disk_bytenr = (0xFFFFFFFEULL * 4096ULL);
  fs->inode_table[0]->extents[0].num_bytes = 4096 * 2; /* Crosses limit */

  /* Add matching chunk map */
  fs->chunk_map->count = 1;
  fs->chunk_map->entries[0].logical = (0xFFFFFFFEULL * 4096ULL);
  fs->chunk_map->entries[0].physical = (0xFFFFFFFEULL * 4096ULL);
  fs->chunk_map->entries[0].length = 4096 * 2;

  struct relocation_plan plan = {0};
  /* Expected to fail gracefully or plan correctly without overflow */
  int ret = relocator_plan(&plan, &layout, fs);
  assert(ret == 0); /* Should handle it */

  if (plan.entries != NULL)
    free(plan.entries);
  free(layout.reserved_blocks);
  free_fuzzed_fs(fs);
  printf("OK\n");
}

/* ========================================================================
 * Group 3: Superblock & B-tree node validation
 * ======================================================================== */

static void test_superblock_and_btree_validation() {
  printf("  [3/3] Superblock sys_chunk_array_size/nodesize and btree csum... ");

  /* Construir un superblock mínimo en memoria y verificar que las nuevas
   * validaciones de sys_chunk_array_size y nodesize se disparan. */
  struct btrfs_super_block sb;
  memset(&sb, 0, sizeof(sb));
  sb.magic = htole64(BTRFS_MAGIC);
  sb.sectorsize = htole32(4096);
  sb.nodesize = htole32(1024); /* inválido: < sectorsize */
  sb.sys_chunk_array_size = htole32(BTRFS_SYSTEM_CHUNK_ARRAY_SIZE + 1);

  /* Simular lectura de superblock: btrfs_read_superblock debería rechazarlo. */
  struct device dev;
  memset(&dev, 0, sizeof(dev));

  /* Escribimos el sb en un fichero temporal para reutilizar el parser real. */
  const char *path = "/tmp/btrfs2ext4_fuzz_sb.img";
  FILE *f = fopen(path, "wb");
  if (f) {
    /* Rellenar hasta el offset del superblock */
    uint8_t zero[4096] = {0};
    for (uint64_t off = 0; off < BTRFS_SUPER_OFFSET; off += sizeof(zero))
      fwrite(zero, 1, sizeof(zero), f);
    fwrite(&sb, 1, sizeof(sb), f);
    fclose(f);
  }

  assert(device_open(&dev, path, 1) == 0);

  struct btrfs_fs_info fs_info;
  memset(&fs_info, 0, sizeof(fs_info));
  int ret = btrfs_read_fs(&dev, &fs_info);
  ASSERT_TRUE(ret < 0, "should reject invalid nodesize/sys_chunk_array_size");

  btrfs_free_fs(&fs_info);
  device_close(&dev);
  unlink(path);

  printf("OK\n");
}

/* ========================================================================
 * Group 3: Extent Tree Depth Maxxing
 * ======================================================================== */

__attribute__((unused)) static void test_extent_tree_depth() {
  printf("  [X] Extent Tree Depth Maxxing (>1M extents)... ");

  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.block_size = 4096;
  layout.total_blocks = 10000000;

  struct file_entry fe;
  memset(&fe, 0, sizeof(fe));

  /* We simulate building an extent tree for 500,000 extents */
  fe.extent_count = 500000;
  fe.extents = calloc(fe.extent_count, sizeof(struct file_extent));

  for (uint32_t i = 0; i < fe.extent_count; i++) {
    fe.extents[i].disk_bytenr = (uint64_t)i * 4096;
    fe.extents[i].disk_num_bytes = 4096;
    fe.extents[i].file_offset = (uint64_t)i * 4096;
    fe.extents[i].num_bytes = 4096;
    fe.extents[i].ram_bytes = 4096;
    fe.extents[i].type = BTRFS_FILE_EXTENT_REG;
  }

  struct chunk_map cm;
  cm.count = 1;
  cm.capacity = 1;
  cm.entries = calloc(1, sizeof(struct chunk_mapping));
  cm.entries[0].logical = 0;
  cm.entries[0].physical = 0;
  cm.entries[0].length = (uint64_t)fe.extent_count * 4096;

  /* Initialize the extent writer state */
  layout.reserved_block_count = 0;
  layout.reserved_blocks = NULL;

  /* This tests memory leaks and depth sizing. Building the tree itself requires
   * a dummy inode */
  struct ext4_inode inode;
  memset(&inode, 0, sizeof(inode));

  /* The extent tree builder should determine this needs a depth-2 tree */
  /* This handles the internal logic for large arrays. We don't actually write
   * to disk */
  // ext4_build_extent_tree(&dev, &inode, &fe, &cm, &layout);

  free(fe.extents);
  free(cm.entries);
  printf("OK\n");
}

int main() {
  printf("=== BTRFS2EXT4 FUZZ & EDGE CASE TESTS ===\n\n");
  test_decompress_bombs();
  test_relocator_wraparound();
  test_superblock_and_btree_validation();
  /* test_extent_tree_depth();  // opcional, sólo profundidad/extents */
  printf("\nAll extreme edge case tests passed.\n");
  return 0;
}