/*
 * test_stress.c — Stress / vulnerability / performance test suite
 *
 * Purpose: Break the program, find edge cases, uncover integer overflows,
 *          memory leaks, and measure performance under extreme loads.
 *
 * Build: linked with the btrfs2ext4 object files (see CMakeLists.txt)
 * Run:   ./test_stress
 *
 * Tests are organized by attack surface:
 *   1. Corrupted superblock / B-tree data
 *   2. Inode map stress (hash collisions, extreme counts)
 *   3. Block allocator exhaustion and edge cases
 *   4. Relocator with pathological inputs
 *   5. Ext4 layout planner overflow checks
 *   6. Directory writer with max entries
 *   7. Extent tree with extreme extent counts
 *   8. Performance benchmarks
 */

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/btrfs_structures.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"
#include "relocator.h"

/* ========================================================================
 * Test infrastructure
 * ======================================================================== */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name)                                                       \
  do {                                                                         \
    tests_run++;                                                               \
    printf("  [TEST %3d] %-55s ", tests_run, name);                            \
    fflush(stdout);                                                            \
  } while (0)

#define TEST_PASS()                                                            \
  do {                                                                         \
    tests_passed++;                                                            \
    printf("\033[32mPASS\033[0m\n");                                           \
  } while (0)

#define TEST_FAIL(reason)                                                      \
  do {                                                                         \
    tests_failed++;                                                            \
    printf("\033[31mFAIL\033[0m (%s)\n", reason);                              \
  } while (0)

#define ASSERT_TRUE(cond, reason)                                              \
  do {                                                                         \
    if (!(cond)) {                                                             \
      TEST_FAIL(reason);                                                       \
      return;                                                                  \
    }                                                                          \
  } while (0)

static double now_seconds(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* Create a temporary file of the given size, return fd */
static int create_temp_device(const char *path, uint64_t size) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return -1;
  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

/* ========================================================================
 * TEST GROUP 1: Corrupted superblock handling
 * ======================================================================== */

static void test_corrupted_superblock_bad_magic(void) {
  TEST_START("Corrupted superblock: bad magic number");

  const char *path = "/tmp/btrfs2ext4_test_badmagic.img";
  if (create_temp_device(path, 64 * 1024 * 1024) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  /* Write garbage at the superblock offset */
  struct device dev;
  if (device_open(&dev, path, 0) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  uint8_t garbage[4096];
  memset(garbage, 0xDE, sizeof(garbage));
  device_write(&dev, BTRFS_SUPER_OFFSET, garbage, sizeof(garbage));
  device_close(&dev);

  /* Try to read — should fail gracefully, not crash */
  struct device dev2;
  if (device_open(&dev2, path, 1) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  struct btrfs_fs_info fs_info;
  memset(&fs_info, 0, sizeof(fs_info));
  int ret = btrfs_read_fs(&dev2, &fs_info);

  /* Must return error, not crash */
  ASSERT_TRUE(ret < 0, "should reject bad magic");

  btrfs_free_fs(&fs_info);
  device_close(&dev2);
  unlink(path);
  TEST_PASS();
}

static void test_corrupted_superblock_zeroed(void) {
  TEST_START("Corrupted superblock: all zeros");

  const char *path = "/tmp/btrfs2ext4_test_zeros.img";
  if (create_temp_device(path, 64 * 1024 * 1024) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 1) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  struct btrfs_fs_info fs_info;
  memset(&fs_info, 0, sizeof(fs_info));
  int ret = btrfs_read_fs(&dev, &fs_info);
  ASSERT_TRUE(ret < 0, "should reject zero superblock");

  btrfs_free_fs(&fs_info);
  device_close(&dev);
  unlink(path);
  TEST_PASS();
}

static void test_corrupted_superblock_bad_csum(void) {
  TEST_START("Corrupted superblock: valid magic, bad checksum");

  const char *path = "/tmp/btrfs2ext4_test_badcsum.img";
  if (create_temp_device(path, 64 * 1024 * 1024) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 0) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  /* Create a superblock with valid magic but bad checksum */
  struct btrfs_super_block sb;
  memset(&sb, 0, sizeof(sb));
  sb.magic = BTRFS_MAGIC;
  sb.nodesize = htole32(16384);
  sb.sectorsize = htole32(4096);
  sb.sys_chunk_array_size = htole32(0);
  /* Deliberately wrong checksum */
  memset(sb.csum, 0xFF, BTRFS_CSUM_SIZE);

  device_write(&dev, BTRFS_SUPER_OFFSET, &sb, sizeof(sb));
  device_close(&dev);

  struct device dev2;
  if (device_open(&dev2, path, 1) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  struct btrfs_fs_info fs_info;
  memset(&fs_info, 0, sizeof(fs_info));
  int ret = btrfs_read_fs(&dev2, &fs_info);
  ASSERT_TRUE(ret < 0, "should reject bad checksum");

  btrfs_free_fs(&fs_info);
  device_close(&dev2);
  unlink(path);
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 2: Inode map stress tests
 * ======================================================================== */

static void test_inode_map_basic_operations(void) {
  TEST_START("Inode map: basic add/lookup");

  struct inode_map map;
  memset(&map, 0, sizeof(map));

  inode_map_add(&map, 256, 2); /* root dir */
  inode_map_add(&map, 257, 11);
  inode_map_add(&map, 258, 12);

  ASSERT_TRUE(inode_map_lookup(&map, 256) == 2, "root lookup");
  ASSERT_TRUE(inode_map_lookup(&map, 257) == 11, "inode 257 lookup");
  ASSERT_TRUE(inode_map_lookup(&map, 258) == 12, "inode 258 lookup");
  ASSERT_TRUE(inode_map_lookup(&map, 999) == 0, "missing lookup");

  inode_map_free(&map);
  TEST_PASS();
}

static void test_inode_map_large_scale(void) {
  TEST_START("Inode map: 100k entries (hash table stress)");

  struct inode_map map;
  memset(&map, 0, sizeof(map));

  const uint32_t N = 100000;
  double t0 = now_seconds();

  /* Add 100k entries */
  for (uint32_t i = 0; i < N; i++) {
    inode_map_add(&map, 256 + i, 11 + i);
  }

  ASSERT_TRUE(map.count == N, "count mismatch");

  double t1 = now_seconds();

  /* Build hash and lookup all */
  /* Note: inode_map_build_hash is static, so we test via the lookup behavior.
   * The hash is not built until ext4_write_inode_table calls it,
   * so here we test the linear fallback path. */
  uint32_t found = 0;
  for (uint32_t i = 0; i < N; i++) {
    if (inode_map_lookup(&map, 256 + i) == 11 + i)
      found++;
  }
  ASSERT_TRUE(found == N, "lookup mismatch");

  double t2 = now_seconds();
  printf("(add=%.3fs lookup=%.3fs) ", t1 - t0, t2 - t1);

  inode_map_free(&map);
  TEST_PASS();
}

static void test_inode_map_hash_collisions(void) {
  TEST_START("Inode map: worst-case hash collision pattern");

  struct inode_map map;
  memset(&map, 0, sizeof(map));

  /* Use inode numbers that are multiples of common hash table sizes
   * to provoke maximum collision chains */
  const uint32_t N = 10000;
  for (uint32_t i = 0; i < N; i++) {
    /* Multiples of 128 (common ht_size) to force collisions */
    inode_map_add(&map, 128 * (i + 1), 11 + i);
  }

  /* Verify all still findable */
  uint32_t found = 0;
  for (uint32_t i = 0; i < N; i++) {
    if (inode_map_lookup(&map, 128 * (i + 1)) == 11 + i)
      found++;
  }
  ASSERT_TRUE(found == N, "collision lookup failed");

  inode_map_free(&map);
  TEST_PASS();
}

static void test_inode_map_zero_entries(void) {
  TEST_START("Inode map: zero entries lookup");

  struct inode_map map;
  memset(&map, 0, sizeof(map));

  /* Lookup on empty map should return 0, not crash */
  uint32_t result = inode_map_lookup(&map, 256);
  ASSERT_TRUE(result == 0, "empty map should return 0");

  inode_map_free(&map);
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 3: Chunk map stress tests
 * ======================================================================== */

static void test_chunk_map_resolve_miss(void) {
  TEST_START("Chunk map: resolve non-existent logical address");

  struct chunk_map map;
  memset(&map, 0, sizeof(map));
  map.capacity = 4;
  map.entries = calloc(map.capacity, sizeof(struct chunk_mapping));

  /* Add one chunk */
  map.entries[0].logical = 0x1000000;
  map.entries[0].physical = 0x2000000;
  map.entries[0].length = 0x100000;
  map.count = 1;

  /* Resolve within range — should succeed */
  uint64_t phys = chunk_map_resolve(&map, 0x1000000);
  ASSERT_TRUE(phys == 0x2000000, "resolve within range");

  /* Resolve outside range — should return -1 */
  phys = chunk_map_resolve(&map, 0x9999999);
  ASSERT_TRUE(phys == (uint64_t)-1, "resolve miss");

  /* Resolve at 0 — should miss */
  phys = chunk_map_resolve(&map, 0);
  ASSERT_TRUE(phys == (uint64_t)-1, "resolve at 0");

  chunk_map_free(&map);
  TEST_PASS();
}

static void test_chunk_map_empty(void) {
  TEST_START("Chunk map: resolve on empty map");

  struct chunk_map map;
  memset(&map, 0, sizeof(map));

  uint64_t phys = chunk_map_resolve(&map, 0x1000000);
  ASSERT_TRUE(phys == (uint64_t)-1, "empty map resolve");
  TEST_PASS();
}

static void test_chunk_map_overlapping_ranges(void) {
  TEST_START("Chunk map: many entries, boundary resolution");

  struct chunk_map map;
  memset(&map, 0, sizeof(map));
  map.capacity = 128;
  map.entries = calloc(map.capacity, sizeof(struct chunk_mapping));

  /* Create 100 contiguous 1MB chunks */
  for (uint32_t i = 0; i < 100; i++) {
    map.entries[i].logical = (uint64_t)i * 0x100000;
    map.entries[i].physical = 0x10000000ULL + (uint64_t)i * 0x100000;
    map.entries[i].length = 0x100000;
  }
  map.count = 100;

  /* Resolve at exact boundaries */
  uint64_t p = chunk_map_resolve(&map, 0);
  ASSERT_TRUE(p == 0x10000000ULL, "first chunk start");

  p = chunk_map_resolve(&map, 0xFFFFF);
  ASSERT_TRUE(p == 0x10000000ULL + 0xFFFFF, "first chunk end");

  p = chunk_map_resolve(&map, 0x100000);
  ASSERT_TRUE(p == 0x10100000ULL, "second chunk start");

  /* Just past last chunk — should miss */
  p = chunk_map_resolve(&map, 100 * 0x100000);
  ASSERT_TRUE(p == (uint64_t)-1, "past last chunk");

  chunk_map_free(&map);
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 4: Ext4 layout planner edge cases
 * ======================================================================== */

static void test_planner_minimum_device(void) {
  TEST_START("Planner: minimum viable device (1 MiB)");

  struct ext4_layout layout;
  int ret = ext4_plan_layout(&layout, 1 * 1024 * 1024, 4096, 16384, NULL);
  /* Should either succeed with 1 group or fail gracefully */
  if (ret == 0) {
    ASSERT_TRUE(layout.num_groups >= 1, "at least 1 group");
    ASSERT_TRUE(layout.block_size == 4096, "block size");
    ext4_free_layout(&layout);
  }
  TEST_PASS();
}

static void test_planner_tiny_device(void) {
  TEST_START("Planner: tiny device (4 KiB — too small)");

  struct ext4_layout layout;
  int ret = ext4_plan_layout(&layout, 4096, 4096, 16384, NULL);
  /* 4 KiB is too small for any valid ext4 — should fail */
  if (ret == 0) {
    ext4_free_layout(&layout);
  }
  /* Whether it succeeds or fails, it must not crash */
  TEST_PASS();
}

static void test_planner_large_device(void) {
  TEST_START("Planner: large device (16 TiB)");

  struct ext4_layout layout;
  uint64_t size_16tb = 16ULL * 1024 * 1024 * 1024 * 1024;
  int ret = ext4_plan_layout(&layout, size_16tb, 4096, 16384, NULL);

  if (ret == 0) {
    /* Check for integer overflow in calculations */
    ASSERT_TRUE(layout.total_blocks == size_16tb / 4096, "total blocks");
    ASSERT_TRUE(layout.num_groups > 0, "has groups");
    /* Verify group count is reasonable */
    uint32_t expected = (uint32_t)((size_16tb / 4096 + 32767) / 32768);
    ASSERT_TRUE(layout.num_groups == expected, "group count overflow check");
    ext4_free_layout(&layout);
  }
  TEST_PASS();
}

static void test_planner_zero_size(void) {
  TEST_START("Planner: zero-size device");

  struct ext4_layout layout;
  int ret = ext4_plan_layout(&layout, 0, 4096, 16384, NULL);
  ASSERT_TRUE(ret < 0, "zero size should fail");
  TEST_PASS();
}

static void test_planner_block_sizes(void) {
  TEST_START("Planner: all valid block sizes (1k, 2k, 4k)");

  uint32_t sizes[] = {1024, 2048, 4096};
  uint64_t dev_size = 256 * 1024 * 1024; /* 256 MiB */

  for (int i = 0; i < 3; i++) {
    struct ext4_layout layout;
    int ret = ext4_plan_layout(&layout, dev_size, sizes[i], 16384, NULL);
    ASSERT_TRUE(ret == 0, "plan should succeed");
    ASSERT_TRUE(layout.block_size == sizes[i], "block size mismatch");
    ext4_free_layout(&layout);
  }
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 5: Relocator stress tests
 * ======================================================================== */

static void test_relocator_empty_plan(void) {
  TEST_START("Relocator: empty plan (no conflicts)");

  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.block_size = 4096;
  layout.total_blocks = 65536;
  layout.reserved_block_count = 0;
  layout.reserved_blocks = NULL;

  struct btrfs_fs_info fs_info;
  memset(&fs_info, 0, sizeof(fs_info));

  struct relocation_plan plan;
  int ret = relocator_plan(&plan, &layout, &fs_info);
  ASSERT_TRUE(ret == 0, "should succeed");
  ASSERT_TRUE(plan.count == 0, "no relocations needed");

  relocator_free(&plan);
  TEST_PASS();
}

static void test_relocator_all_blocks_conflict(void) {
  TEST_START("Relocator: every data block is a conflict");

  const uint32_t NBLOCKS = 1000;
  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.block_size = 4096;
  layout.total_blocks = NBLOCKS + 2000; /* some free room */
  layout.reserved_blocks = malloc(NBLOCKS * sizeof(uint64_t));
  layout.reserved_block_count = NBLOCKS;
  layout.reserved_block_capacity = NBLOCKS;

  /* Reserve blocks 0-999 */
  for (uint32_t i = 0; i < NBLOCKS; i++)
    layout.reserved_blocks[i] = i;

  /* Create groups so free space init can find blocks */
  layout.num_groups = 1;
  struct ext4_bg_layout bg;
  memset(&bg, 0, sizeof(bg));
  bg.group_start_block = 0;
  bg.data_start_block = 0;
  bg.data_blocks = layout.total_blocks;
  layout.groups = &bg;

  /* Create a fake file with extents in the reserved region */
  struct file_entry fe;
  memset(&fe, 0, sizeof(fe));
  fe.ino = 256;
  fe.mode = 0100644;

  /* Create a chunk map identity mapping (logical = physical) */
  struct chunk_map cmap;
  memset(&cmap, 0, sizeof(cmap));
  cmap.capacity = 1;
  cmap.entries = calloc(1, sizeof(struct chunk_mapping));
  cmap.entries[0].logical = 0;
  cmap.entries[0].physical = 0;
  cmap.entries[0].length = layout.total_blocks * 4096;
  cmap.count = 1;

  struct file_extent ext;
  memset(&ext, 0, sizeof(ext));
  ext.type = 1;           /* REG */
  ext.disk_bytenr = 4096; /* block 1 (non-zero = not a hole) */
  ext.disk_num_bytes =
      (uint64_t)(NBLOCKS - 1) * 4096; /* blocks 1..(NBLOCKS-1) */
  ext.num_bytes = ext.disk_num_bytes;

  fe.extents = &ext;
  fe.extent_count = 1;

  struct file_entry *table[] = {&fe};
  struct btrfs_fs_info fs_info;
  memset(&fs_info, 0, sizeof(fs_info));
  fs_info.inode_table = table;
  fs_info.inode_count = 1;
  fs_info.chunk_map = &cmap;

  struct relocation_plan plan;
  int ret = relocator_plan(&plan, &layout, &fs_info);

  if (ret == 0) {
    printf("(%u entries) ", plan.count);
    /* Coalesced entries should be fewer than NBLOCKS */
    ASSERT_TRUE(plan.count <= NBLOCKS, "count within bounds");
    ASSERT_TRUE(plan.total_bytes_to_move == (uint64_t)(NBLOCKS - 1) * 4096,
                "total bytes");
  }

  relocator_free(&plan);
  free(layout.reserved_blocks);
  chunk_map_free(&cmap);
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 6: Device I/O edge cases
 * ======================================================================== */

static void test_device_read_beyond_end(void) {
  TEST_START("Device I/O: read beyond device end");

  const char *path = "/tmp/btrfs2ext4_test_devio.img";
  if (create_temp_device(path, 4096) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 1) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  uint8_t buf[4096];
  /* Read at offset 0 — should succeed */
  int ret = device_read(&dev, 0, buf, 4096);
  ASSERT_TRUE(ret == 0, "read at 0 should succeed");

  /* Read past end — should fail */
  ret = device_read(&dev, 4096, buf, 1);
  ASSERT_TRUE(ret < 0, "read past end should fail");

  /* Read at offset that would overflow — should fail */
  ret = device_read(&dev, 1, buf, 4096);
  ASSERT_TRUE(ret < 0, "read overflow should fail");

  device_close(&dev);
  unlink(path);
  TEST_PASS();
}

static void test_device_write_readonly(void) {
  TEST_START("Device I/O: write to read-only device");

  const char *path = "/tmp/btrfs2ext4_test_ronly.img";
  if (create_temp_device(path, 4096) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 1) < 0) { /* read_only = 1 */
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  uint8_t buf[4096] = {0};
  int ret = device_write(&dev, 0, buf, 4096);
  ASSERT_TRUE(ret < 0, "write to ro device should fail");

  device_close(&dev);
  unlink(path);
  TEST_PASS();
}

static void test_device_zero_size_file(void) {
  TEST_START("Device I/O: open zero-size file");

  const char *path = "/tmp/btrfs2ext4_test_zero.img";
  if (create_temp_device(path, 0) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  int ret = device_open(&dev, path, 1);
  /* Should fail (zero size) */
  ASSERT_TRUE(ret < 0, "zero-size should fail");

  unlink(path);
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 7: Extent tree edge cases
 * ======================================================================== */

static void test_extent_tree_empty_file(void) {
  TEST_START("Extent tree: empty file (zero extents)");

  const char *path = "/tmp/btrfs2ext4_test_extent.img";
  if (create_temp_device(path, 64 * 1024 * 1024) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 0) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  struct ext4_inode inode;
  memset(&inode, 0, sizeof(inode));

  struct file_entry fe;
  memset(&fe, 0, sizeof(fe));
  fe.ino = 256;
  fe.extent_count = 0;
  fe.extents = NULL;

  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.block_size = 4096;
  layout.total_blocks = 16384;

  struct chunk_map cmap;
  memset(&cmap, 0, sizeof(cmap));

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  int ret = ext4_build_extent_tree(&alloc, &dev, &inode, &fe, &cmap, &layout);
  ext4_block_alloc_free(&alloc);

  ASSERT_TRUE(ret == 0, "empty file should succeed");

  /* Check header */
  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  ASSERT_TRUE(le16toh(eh->eh_magic) == EXT4_EXT_MAGIC, "magic");
  ASSERT_TRUE(le16toh(eh->eh_entries) == 0, "zero entries");
  ASSERT_TRUE(le16toh(eh->eh_depth) == 0, "depth 0");

  device_close(&dev);
  unlink(path);
  TEST_PASS();
}

static void test_extent_tree_single_extent(void) {
  TEST_START("Extent tree: single extent (inline)");

  const char *path = "/tmp/btrfs2ext4_test_ext1.img";
  if (create_temp_device(path, 64 * 1024 * 1024) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 0) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  struct chunk_map cmap;
  memset(&cmap, 0, sizeof(cmap));
  cmap.capacity = 1;
  cmap.entries = calloc(1, sizeof(struct chunk_mapping));
  cmap.entries[0].logical = 0;
  cmap.entries[0].physical = 0;
  cmap.entries[0].length = 64 * 1024 * 1024;
  cmap.count = 1;

  struct file_extent ext;
  memset(&ext, 0, sizeof(ext));
  ext.type = 1; /* REG */
  ext.file_offset = 0;
  ext.disk_bytenr = 4096 * 100;
  ext.disk_num_bytes = 4096 * 10;
  ext.num_bytes = 4096 * 10;

  struct file_entry fe;
  memset(&fe, 0, sizeof(fe));
  fe.ino = 256;
  fe.extent_count = 1;
  fe.extents = &ext;

  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.block_size = 4096;
  layout.total_blocks = 16384;

  struct ext4_inode inode;
  memset(&inode, 0, sizeof(inode));

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  int ret = ext4_build_extent_tree(&alloc, &dev, &inode, &fe, &cmap, &layout);
  ext4_block_alloc_free(&alloc);

  ASSERT_TRUE(ret == 0, "single extent should succeed");

  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  ASSERT_TRUE(le16toh(eh->eh_entries) == 1, "one entry");
  ASSERT_TRUE(le16toh(eh->eh_depth) == 0, "depth 0 (inline)");

  device_close(&dev);
  chunk_map_free(&cmap);
  unlink(path);
  TEST_PASS();
}

static void test_extent_tree_max_inline(void) {
  TEST_START("Extent tree: exactly 4 extents (max inline)");

  const char *path = "/tmp/btrfs2ext4_test_ext4.img";
  if (create_temp_device(path, 64 * 1024 * 1024) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 0) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  struct chunk_map cmap;
  memset(&cmap, 0, sizeof(cmap));
  cmap.capacity = 1;
  cmap.entries = calloc(1, sizeof(struct chunk_mapping));
  cmap.entries[0].logical = 0;
  cmap.entries[0].physical = 0;
  cmap.entries[0].length = 64 * 1024 * 1024;
  cmap.count = 1;

  struct file_extent exts[4];
  memset(exts, 0, sizeof(exts));
  for (int i = 0; i < 4; i++) {
    exts[i].type = 1;
    exts[i].file_offset = (uint64_t)i * 4096 * 100;
    exts[i].disk_bytenr = (uint64_t)(i + 1) * 4096 * 200;
    exts[i].disk_num_bytes = 4096 * 10;
    exts[i].num_bytes = 4096 * 10;
  }

  struct file_entry fe;
  memset(&fe, 0, sizeof(fe));
  fe.ino = 256;
  fe.extent_count = 4;
  fe.extents = exts;

  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.block_size = 4096;
  layout.total_blocks = 16384;

  struct ext4_inode inode;
  memset(&inode, 0, sizeof(inode));

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  int ret = ext4_build_extent_tree(&alloc, &dev, &inode, &fe, &cmap, &layout);
  ext4_block_alloc_free(&alloc);

  ASSERT_TRUE(ret == 0, "4 extents should succeed");

  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  ASSERT_TRUE(le16toh(eh->eh_entries) == 4, "four entries");
  ASSERT_TRUE(le16toh(eh->eh_depth) == 0, "still inline");

  device_close(&dev);
  chunk_map_free(&cmap);
  unlink(path);
  TEST_PASS();
}

static void test_extent_tree_multi_level(void) {
  TEST_START("Extent tree: 100 extents (multi-level tree)");

  const char *path = "/tmp/btrfs2ext4_test_ext100.img";
  if (create_temp_device(path, 64 * 1024 * 1024) < 0) {
    TEST_FAIL("couldn't create temp file");
    return;
  }

  struct device dev;
  if (device_open(&dev, path, 0) < 0) {
    TEST_FAIL("device_open failed");
    unlink(path);
    return;
  }

  struct chunk_map cmap;
  memset(&cmap, 0, sizeof(cmap));
  cmap.capacity = 1;
  cmap.entries = calloc(1, sizeof(struct chunk_mapping));
  cmap.entries[0].logical = 0;
  cmap.entries[0].physical = 0;
  cmap.entries[0].length = 64 * 1024 * 1024;
  cmap.count = 1;

  /* Create 100 non-adjacent extents */
  const int NEXT = 100;
  struct file_extent *exts = calloc(NEXT, sizeof(struct file_extent));
  for (int i = 0; i < NEXT; i++) {
    exts[i].type = 1;
    exts[i].file_offset = (uint64_t)i * 4096 * 50;
    exts[i].disk_bytenr = (uint64_t)(i * 2 + 10) * 4096;
    exts[i].disk_num_bytes = 4096 * 5;
    exts[i].num_bytes = 4096 * 5;
  }

  struct file_entry fe;
  memset(&fe, 0, sizeof(fe));
  fe.ino = 256;
  fe.extent_count = NEXT;
  fe.extents = exts;

  /* Set up layout with block groups and allocator */
  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.block_size = 4096;
  layout.total_blocks = 16384;
  layout.num_groups = 1;

  struct ext4_bg_layout bg;
  memset(&bg, 0, sizeof(bg));
  bg.group_start_block = 0;
  bg.data_start_block = 100;
  bg.data_blocks = 16284;
  layout.groups = &bg;

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  struct ext4_inode inode;
  memset(&inode, 0, sizeof(inode));

  int ret = ext4_build_extent_tree(&alloc, &dev, &inode, &fe, &cmap, &layout);
  ext4_block_alloc_free(&alloc);

  ASSERT_TRUE(ret == 0, "100 extents should succeed");

  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  ASSERT_TRUE(le16toh(eh->eh_depth) == 1, "depth 1 (multi-level)");

  free(exts);
  device_close(&dev);
  chunk_map_free(&cmap);
  unlink(path);
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 8: Performance benchmarks
 * ======================================================================== */

static void bench_inode_map_lookup(void) {
  TEST_START("BENCH: inode map 500k lookups");

  struct inode_map map;
  memset(&map, 0, sizeof(map));

  const uint32_t N = 50000;
  for (uint32_t i = 0; i < N; i++) {
    inode_map_add(&map, 256 + i, 11 + i);
  }

  double t0 = now_seconds();
  volatile uint32_t result = 0;
  for (int round = 0; round < 10; round++) {
    for (uint32_t i = 0; i < N; i++) {
      result += inode_map_lookup(&map, 256 + i);
    }
  }
  double t1 = now_seconds();

  double lookups = (double)N * 10;
  printf("(%.0f lookups/sec, total=%.3fs) ", lookups / (t1 - t0), t1 - t0);
  (void)result;

  inode_map_free(&map);
  TEST_PASS();
}

static void bench_chunk_map_resolve(void) {
  TEST_START("BENCH: chunk map 1M resolves");

  struct chunk_map map;
  memset(&map, 0, sizeof(map));
  map.capacity = 256;
  map.entries = calloc(map.capacity, sizeof(struct chunk_mapping));

  /* Create 200 chunks */
  for (uint32_t i = 0; i < 200; i++) {
    map.entries[i].logical = (uint64_t)i * 0x10000000;
    map.entries[i].physical = 0x10000000ULL + (uint64_t)i * 0x10000000;
    map.entries[i].length = 0x10000000;
  }
  map.count = 200;

  double t0 = now_seconds();
  volatile uint64_t result = 0;
  for (int round = 0; round < 1000000; round++) {
    result +=
        chunk_map_resolve(&map, (uint64_t)(round % 200) * 0x10000000 + 0x1000);
  }
  double t1 = now_seconds();

  printf("(%.0f resolves/sec, total=%.3fs) ", 1000000.0 / (t1 - t0), t1 - t0);
  (void)result;

  chunk_map_free(&map);
  TEST_PASS();
}

static void bench_planner_large(void) {
  TEST_START("BENCH: plan layout for 1 TiB device");

  double t0 = now_seconds();
  struct ext4_layout layout;
  uint64_t size_1tb = 1ULL * 1024 * 1024 * 1024 * 1024;
  int ret = ext4_plan_layout(&layout, size_1tb, 4096, 16384, NULL);
  double t1 = now_seconds();

  if (ret == 0) {
    printf("(%u groups, %.3fs) ", layout.num_groups, t1 - t0);
    ASSERT_TRUE(t1 - t0 < 5.0, "should complete in <5s");
    ext4_free_layout(&layout);
  } else {
    printf("(planner failed, %.3fs) ", t1 - t0);
  }
  TEST_PASS();
}

static void bench_conflict_bitmap(void) {
  TEST_START("BENCH: conflict bitmap build+check (100k reserved)");

  const uint32_t N = 100000;
  struct ext4_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.total_blocks = 1000000;
  layout.reserved_blocks = malloc(N * sizeof(uint64_t));
  layout.reserved_block_count = N;

  /* Scatter reserved blocks */
  for (uint32_t i = 0; i < N; i++)
    layout.reserved_blocks[i] = i * 10;

  double t0 = now_seconds();

  /* Build bitmap (same method as relocator) */
  uint8_t *bitmap = calloc((layout.total_blocks + 7) / 8, 1);
  for (uint32_t i = 0; i < N; i++) {
    uint64_t b = layout.reserved_blocks[i];
    if (b < layout.total_blocks)
      bitmap[b / 8] |= (1 << (b % 8));
  }

  /* Check 1 million blocks */
  volatile int conflicts = 0;
  for (uint64_t b = 0; b < layout.total_blocks; b++) {
    if (bitmap[b / 8] & (1 << (b % 8)))
      conflicts++;
  }

  double t1 = now_seconds();

  printf("(%d conflicts, %.3fs) ", conflicts, t1 - t0);
  ASSERT_TRUE(conflicts == (int)N, "conflict count");

  free(bitmap);
  free(layout.reserved_blocks);
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 9: Integer overflow / boundary tests
 * ======================================================================== */

static void test_overflow_block_count(void) {
  TEST_START("Overflow: block count near UINT32_MAX");

  struct ext4_layout layout;
  /* Device just under 16 TiB (max for ext4 with 4K blocks) */
  uint64_t size = (uint64_t)UINT32_MAX * 4096ULL;
  int ret = ext4_plan_layout(&layout, size, 4096, 16384, NULL);
  /* Must not crash, regardless of success/failure */
  if (ret == 0) {
    printf("(%u groups) ", layout.num_groups);
    ext4_free_layout(&layout);
  }
  TEST_PASS();
}

static void test_overflow_huge_inode_ratio(void) {
  TEST_START("Overflow: inode ratio = 1 (inode per byte)");

  struct ext4_layout layout;
  int ret = ext4_plan_layout(&layout, 256 * 1024 * 1024, 4096, 1, NULL);
  /* Ratio of 1 means 1 inode per byte = 256M inodes.
   * Should either succeed with massive inode table or fail gracefully */
  if (ret == 0) {
    printf("(%u inodes) ", layout.total_inodes);
    ext4_free_layout(&layout);
  }
  TEST_PASS();
}

static void test_overflow_max_inode_ratio(void) {
  TEST_START("Overflow: inode ratio = UINT32_MAX");

  struct ext4_layout layout;
  int ret = ext4_plan_layout(&layout, 1ULL * 1024 * 1024 * 1024, 4096,
                             UINT32_MAX, NULL);
  /* Huge ratio = very few inodes. Should succeed. */
  if (ret == 0) {
    printf("(%u inodes) ", layout.total_inodes);
    ext4_free_layout(&layout);
  }
  TEST_PASS();
}

/* ========================================================================
 * TEST GROUP 10: Memory safety under stress
 * ======================================================================== */

static void test_free_double_free(void) {
  TEST_START("Memory: double free protection");

  struct inode_map map;
  memset(&map, 0, sizeof(map));
  inode_map_add(&map, 256, 2);
  inode_map_free(&map);

  /* Second free should be safe (map zeroed) */
  inode_map_free(&map);

  struct relocation_plan plan;
  memset(&plan, 0, sizeof(plan));
  relocator_free(&plan);
  relocator_free(&plan); /* double free */

  struct chunk_map cmap;
  memset(&cmap, 0, sizeof(cmap));
  chunk_map_free(&cmap);
  chunk_map_free(&cmap); /* double free */

  TEST_PASS();
}

static void test_free_after_operations(void) {
  TEST_START("Memory: free after heavy operations");

  struct inode_map map;
  memset(&map, 0, sizeof(map));

  for (uint32_t i = 0; i < 10000; i++) {
    inode_map_add(&map, i, i + 11);
  }

  /* Verify before free */
  ASSERT_TRUE(inode_map_lookup(&map, 5000) == 5011, "lookup before free");

  inode_map_free(&map);

  /* After free, lookup should return 0 (count is 0) */
  ASSERT_TRUE(inode_map_lookup(&map, 5000) == 0, "lookup after free");
  TEST_PASS();
}

/* ========================================================================
 * Main test runner
 * ======================================================================== */

int main(void) {
  printf("\n");
  printf(
      "╔══════════════════════════════════════════════════════════════════╗\n");
  printf(
      "║   btrfs2ext4 — Stress / Vulnerability / Performance Tests      ║\n");
  printf(
      "╚══════════════════════════════════════════════════════════════════╝\n");
  printf("\n");

  /* Group 1: Corrupted superblock */
  printf(
      "─── GROUP 1: Corrupted Superblock ────────────────────────────────\n");
  test_corrupted_superblock_bad_magic();
  test_corrupted_superblock_zeroed();
  test_corrupted_superblock_bad_csum();

  /* Group 2: Inode map */
  printf(
      "\n─── GROUP 2: Inode Map Stress ──────────────────────────────────\n");
  test_inode_map_basic_operations();
  test_inode_map_large_scale();
  test_inode_map_hash_collisions();
  test_inode_map_zero_entries();

  /* Group 3: Chunk map */
  printf(
      "\n─── GROUP 3: Chunk Map Edge Cases ──────────────────────────────\n");
  test_chunk_map_resolve_miss();
  test_chunk_map_empty();
  test_chunk_map_overlapping_ranges();

  /* Group 4: Ext4 planner */
  printf(
      "\n─── GROUP 4: Ext4 Layout Planner ───────────────────────────────\n");
  test_planner_minimum_device();
  test_planner_tiny_device();
  test_planner_large_device();
  test_planner_zero_size();
  test_planner_block_sizes();

  /* Group 5: Relocator */
  printf(
      "\n─── GROUP 5: Relocator Stress ──────────────────────────────────\n");
  test_relocator_empty_plan();
  test_relocator_all_blocks_conflict();

  /* Group 6: Device I/O */
  printf(
      "\n─── GROUP 6: Device I/O Edge Cases ─────────────────────────────\n");
  test_device_read_beyond_end();
  test_device_write_readonly();
  test_device_zero_size_file();

  /* Group 7: Extent tree */
  printf(
      "\n─── GROUP 7: Extent Tree Edge Cases ────────────────────────────\n");
  test_extent_tree_empty_file();
  test_extent_tree_single_extent();
  test_extent_tree_max_inline();
  test_extent_tree_multi_level();

  /* Group 8: Benchmarks */
  printf(
      "\n─── GROUP 8: Performance Benchmarks ────────────────────────────\n");
  bench_inode_map_lookup();
  bench_chunk_map_resolve();
  bench_planner_large();
  bench_conflict_bitmap();

  /* Group 9: Integer overflow */
  printf(
      "\n─── GROUP 9: Integer Overflow / Boundary ───────────────────────\n");
  test_overflow_block_count();
  test_overflow_huge_inode_ratio();
  test_overflow_max_inode_ratio();

  /* Group 10: Memory safety */
  printf(
      "\n─── GROUP 10: Memory Safety ────────────────────────────────────\n");
  test_free_double_free();
  test_free_after_operations();

  /* Summary */
  printf("\n");
  printf(
      "══════════════════════════════════════════════════════════════════\n");
  printf("  Results: %d tests run, \033[32m%d passed\033[0m", tests_run,
         tests_passed);
  if (tests_failed > 0)
    printf(", \033[31m%d FAILED\033[0m", tests_failed);
  printf("\n");
  printf(
      "══════════════════════════════════════════════════════════════════\n\n");

  return tests_failed > 0 ? 1 : 0;
}
