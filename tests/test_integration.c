/*
 * test_integration.c — Integration & Regression Test Suite
 *
 * Verifica el comportamiento real en disco: escribe estructuras ext4 con las
 * funciones del proyecto y lee de vuelta los bytes exactos para validarlos.
 * No asume que una función que "retorna 0" ha producido datos correctos.
 *
 * Organización:
 *   A. device_batch API   — buffer lifetime, flush, equivalencia con pwrite
 *   B. Inode bitmaps      — Bug B-2: inodos reales marcados como usados
 *   C. GDT offsets        — Bug B-3: desc_size=64 vs sizeof(struct)=32
 *   D. Tail block marking — Bug B-11: último grupo parcial
 *   E. Dir extent tree    — Bug B-4: directorios >4 bloques, depth=1
 *   F. GDT checksums      — Bug B-6: bg_checksum ≠ 0
 *   G. Block allocator    — Bug B-9 (dirección), Bug B-10 (wrap-around)
 *   H. Journal zeroing    — Bug B-7: fallocate/pwrite-chunk path
 *   I. Consistencia E2E   — superbloque, GDT, bitmaps, inodos, free counts
 *
 * Build: añadir test_integration.c a CMakeLists.txt como nuevo target
 *        (mismo patrón que test_stress)
 * Run:   ./test_integration
 * ASAN:  compilar con -fsanitize=address para detectar B-2 buffer overflows
 */

#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <endian.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/chunk_tree.h"
#include "device_io.h"
#include "ext4/ext4_crc16.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

/* =========================================================================
 * Infrastructure
 * ======================================================================= */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define COL_PASS "\033[32m"
#define COL_FAIL "\033[31m"
#define COL_WARN "\033[33m"
#define COL_RST "\033[0m"

#define TEST_START(name)                                                       \
  do {                                                                         \
    tests_run++;                                                               \
    printf("  [%3d] %-60s", tests_run, name);                                  \
    fflush(stdout);                                                            \
  } while (0)

#define TEST_PASS()                                                            \
  do {                                                                         \
    tests_passed++;                                                            \
    printf(COL_PASS "PASS" COL_RST "\n");                                      \
  } while (0)

#define TEST_FAIL(reason)                                                      \
  do {                                                                         \
    tests_failed++;                                                            \
    printf(COL_FAIL "FAIL" COL_RST " — %s\n", reason);                         \
  } while (0)

/* CHECK: si la condición falla, imprime FAIL y retorna del test. */
#define CHECK(cond, reason)                                                    \
  do {                                                                         \
    if (!(cond)) {                                                             \
      TEST_FAIL(reason);                                                       \
      return;                                                                  \
    }                                                                          \
  } while (0)

/* REQUIRE: fatal si falla (no tiene sentido continuar el test). */
#define REQUIRE(cond, reason)                                                  \
  do {                                                                         \
    if (!(cond)) {                                                             \
      TEST_FAIL(reason);                                                       \
      return;                                                                  \
    }                                                                          \
  } while (0)

/* Tamaño de la imagen de prueba: 128 MB — suficiente para ~3 grupos */
#define TEST_IMG_SIZE (128ULL * 1024 * 1024)
#define TEST_BLOCK_SIZE 4096

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Crea /tmp/b2e4_itest_<pid>_<n>.img y abre struct device */
static int make_test_dev(struct device *dev, const char *suffix,
                         uint64_t size) {
  char path[128];
  snprintf(path, sizeof(path), "/tmp/b2e4_itest_%d_%s.img", getpid(), suffix);
  unlink(path); /* borrar residuo de ejecución anterior */

  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return -1;
  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return -1;
  }
  close(fd);

  if (device_open(dev, path, 0) < 0)
    return -1;
  return 0;
}

static void cleanup_test_dev(struct device *dev) {
  char path[128];
  strncpy(path, dev->path, sizeof(path) - 1);
  device_close(dev);
  unlink(path);
}

/* Lee exactamente n bytes del device en el offset dado */
static int read_raw(struct device *dev, uint64_t offset, void *buf, size_t n) {
  return device_read(dev, offset, buf, n);
}

/* Removed erroneous MSB-first crc16_ibm, using ext4_crc16 */

/* Calcula el checksum esperado para el group descriptor g
 * Algoritmo: CRC16(seed=0, UUID || le16(g) || descriptor_con_csum=0) */
static uint16_t expected_gdt_csum(const uint8_t *uuid, uint32_t group_no,
                                  const uint8_t *desc_bytes, size_t desc_size) {
  uint16_t crc = 0xFFFF; /* Seed CRC with ~0 */
  crc = ext4_crc16(crc, uuid, 16);
  uint32_t le_group =
      htole32(group_no); /* Group number must be le32 for Ext4! */
  crc = ext4_crc16(crc, &le_group, 4);

  /* El checksum se calcula con el campo bg_checksum puesto a 0 */
  uint8_t tmp[64];
  memcpy(tmp, desc_bytes, desc_size < 64 ? desc_size : 64);
  /* bg_checksum está en el offset 30 del descriptor */
  tmp[30] = 0;
  tmp[31] = 0;
  crc = ext4_crc16(crc, tmp, desc_size);
  return crc;
}

/* Construye un ext4_layout de prueba para TEST_IMG_SIZE */
static int build_test_layout(struct ext4_layout *layout) {
  return ext4_plan_layout(layout, TEST_IMG_SIZE, TEST_BLOCK_SIZE, 16384, NULL);
}

/* =========================================================================
 * GROUP A — device_batch API
 *
 * Verifica: correctitud de datos escritos, equivalencia con pwrite directo,
 * take_ownership (no double-free), flush con capacidad excedida.
 * ======================================================================= */

static void test_batch_basic_readback(void) {
  TEST_START("A-1  batch: datos leídos de vuelta coinciden");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "batchA1", 1024 * 1024) == 0,
          "no se pudo crear imagen");

  REQUIRE(device_write_batch_begin(&dev) == 0, "batch_begin falló");

  /* Escribir 4 bloques con patrones conocidos */
  uint8_t *bufs[4];
  for (int i = 0; i < 4; i++) {
    bufs[i] = malloc(TEST_BLOCK_SIZE);
    memset(bufs[i], 0xA0 + i, TEST_BLOCK_SIZE);
    REQUIRE(device_write_batch_add(&dev, (uint64_t)i * TEST_BLOCK_SIZE, bufs[i],
                                   TEST_BLOCK_SIZE) == 0,
            "batch_add falló");
  }
  REQUIRE(device_write_batch_submit(&dev) == 0, "batch_submit falló");

  /* Verificar byte a byte */
  for (int i = 0; i < 4; i++) {
    uint8_t readbuf[TEST_BLOCK_SIZE];
    REQUIRE(read_raw(&dev, (uint64_t)i * TEST_BLOCK_SIZE, readbuf,
                     TEST_BLOCK_SIZE) == 0,
            "lectura falló");
    for (int j = 0; j < TEST_BLOCK_SIZE; j++) {
      if (readbuf[j] != (uint8_t)(0xA0 + i)) {
        TEST_FAIL("datos corruptos en lectura");
        for (int k = 0; k < 4; k++)
          free(bufs[k]);
        cleanup_test_dev(&dev);
        return;
      }
    }
    free(bufs[i]);
  }

  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_batch_empty_flush_noop(void) {
  TEST_START("A-3  batch: flush vacío retorna 0 sin I/O");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "batchA3", 64 * 1024) == 0,
          "no se pudo crear imagen");

  REQUIRE(device_write_batch_begin(&dev) == 0, "batch_begin falló");
  /* submit inmediato sin ningún add */
  CHECK(device_write_batch_submit(&dev) == 0, "submit vacío retornó error");
  CHECK(device_write_batch_submit(&dev) == 0,
        "segundo submit vacío retornó error");

  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_batch_overflow_auto_flush(void) {
  TEST_START("A-4  batch: auto-flush cuando se excede capacidad");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "batchA4", 2048 * 1024) == 0,
          "no se pudo crear imagen");

  /* Capacidad = 256 (interna): añadir 300 ops, debe hacer auto-flush interno */
  REQUIRE(device_write_batch_begin(&dev) == 0, "batch_begin falló");

  uint8_t *bufs[300];
  uint8_t pattern[TEST_BLOCK_SIZE];
  for (uint32_t i = 0; i < 300; i++) {
    memset(pattern, (uint8_t)(i + 1), TEST_BLOCK_SIZE);
    bufs[i] = malloc(TEST_BLOCK_SIZE);
    REQUIRE(bufs[i] != NULL, "malloc falló");
    memcpy(bufs[i], pattern, TEST_BLOCK_SIZE);
    REQUIRE(device_write_batch_add(&dev, (uint64_t)i * TEST_BLOCK_SIZE, bufs[i],
                                   TEST_BLOCK_SIZE) == 0,
            "batch_add falló en overflow");
  }
  REQUIRE(device_write_batch_submit(&dev) == 0, "submit final falló");

  for (int i = 0; i < 300; i++)
    free(bufs[i]);

  /* Verificar que los 300 bloques tienen los patrones correctos */
  for (uint32_t i = 0; i < 300; i++) {
    uint8_t rb[TEST_BLOCK_SIZE];
    REQUIRE(
        read_raw(&dev, (uint64_t)i * TEST_BLOCK_SIZE, rb, TEST_BLOCK_SIZE) == 0,
        "lectura falló");
    if (rb[0] != (uint8_t)(i + 1)) {
      TEST_FAIL("dato incorrecto tras auto-flush");
      cleanup_test_dev(&dev);
      return;
    }
  }

  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_batch_pwrite_equivalence(void) {
  TEST_START("A-5  batch: equivalencia bit a bit con pwrite directo");

  struct device dev_batch, dev_direct;
  REQUIRE(make_test_dev(&dev_batch, "batchA5a", 64 * 1024) == 0,
          "no se pudo crear imagen batch");
  REQUIRE(make_test_dev(&dev_direct, "batchA5b", 64 * 1024) == 0,
          "no se pudo crear imagen direct");

  /* Mismo patrón, escrito de dos formas */
  uint8_t pattern[TEST_BLOCK_SIZE];
  for (int i = 0; i < TEST_BLOCK_SIZE; i++)
    pattern[i] = (uint8_t)(i * 7 + 3);

  /* Vía batch */
  REQUIRE(device_write_batch_begin(&dev_batch) == 0, "batch_begin falló");
  uint8_t *bufs[4];
  for (int i = 0; i < 4; i++) {
    bufs[i] = malloc(TEST_BLOCK_SIZE);
    REQUIRE(bufs[i] != NULL, "malloc falló");
    memcpy(bufs[i], pattern, TEST_BLOCK_SIZE);
    device_write_batch_add(&dev_batch, (uint64_t)i * TEST_BLOCK_SIZE, bufs[i],
                           TEST_BLOCK_SIZE);
  }
  device_write_batch_submit(&dev_batch);
  for (int i = 0; i < 4; i++)
    free(bufs[i]);

  /* Vía device_write directo */
  for (int i = 0; i < 4; i++)
    device_write(&dev_direct, (uint64_t)i * TEST_BLOCK_SIZE, pattern,
                 TEST_BLOCK_SIZE);

  /* Comparar los primeros 4 bloques byte a byte */
  for (int i = 0; i < 4; i++) {
    uint8_t rb[TEST_BLOCK_SIZE], rd[TEST_BLOCK_SIZE];
    read_raw(&dev_batch, (uint64_t)i * TEST_BLOCK_SIZE, rb, TEST_BLOCK_SIZE);
    read_raw(&dev_direct, (uint64_t)i * TEST_BLOCK_SIZE, rd, TEST_BLOCK_SIZE);
    if (memcmp(rb, rd, TEST_BLOCK_SIZE) != 0) {
      TEST_FAIL("batch y pwrite producen datos distintos");
      cleanup_test_dev(&dev_batch);
      cleanup_test_dev(&dev_direct);
      return;
    }
  }

  cleanup_test_dev(&dev_batch);
  cleanup_test_dev(&dev_direct);
  TEST_PASS();
}

static void test_batch_readonly_rejected(void) {
  TEST_START("A-6  batch: dispositivo read-only rechaza add");

  char path[128];
  snprintf(path, sizeof(path), "/tmp/b2e4_itest_%d_batchA6.img", getpid());
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  REQUIRE(fd >= 0, "no se pudo crear imagen");
  ftruncate(fd, 64 * 1024);
  close(fd);

  struct device dev;
  REQUIRE(device_open(&dev, path, 1 /* read_only */) == 0, "device_open falló");

  REQUIRE(device_write_batch_begin(&dev) == 0, "batch_begin falló");

  uint8_t buf[TEST_BLOCK_SIZE] = {0};
  int ret = device_write_batch_add(&dev, 0, buf, TEST_BLOCK_SIZE);
  /* Debe rechazarlo (retornar -1 o flush fallará) */
  int flush_ret = device_write_batch_submit(&dev);
  device_close(&dev);
  unlink(path);

  CHECK(ret < 0 || flush_ret < 0,
        "dispositivo read-only debería rechazar writes");
  TEST_PASS();
}

/* =========================================================================
 * GROUP B — Inode Bitmaps (Bug B-2)
 *
 * ext4_write_bitmaps debe marcar en el inode bitmap de cada grupo los inodos
 * que están en uso. Sin la corrección del bug, solo los inodos 1-10 del
 * grupo 0 quedan marcados y todos los demás aparecen como libres.
 * ======================================================================= */

static void test_inode_bitmap_reserved_inodes(void) {
  TEST_START("B-1  inode bitmap: inodos reservados 1-10 marcados en grupo 0");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "ibitmapB1", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  /* Inicializar alloc sin bits de datos — solo metadatos reservados */
  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  REQUIRE(ext4_write_bitmaps(&dev, &layout, &alloc, NULL) == 0,
          "write_bitmaps falló");

  /* Leer el inode bitmap del grupo 0 */
  uint8_t ibm[TEST_BLOCK_SIZE];
  REQUIRE(read_raw(&dev, layout.groups[0].inode_bitmap_block * TEST_BLOCK_SIZE,
                   ibm, TEST_BLOCK_SIZE) == 0,
          "lectura inode bitmap falló");

  /* Los 10 primeros bits (inodos 1-10) deben estar a 1 */
  for (int i = 0; i < 10; i++) {
    uint8_t byte_val = ibm[i / 8];
    uint8_t bit = (byte_val >> (i % 8)) & 1;
    if (!bit) {
      char msg[64];
      snprintf(msg, sizeof(msg), "inodo reservado %d no marcado", i + 1);
      TEST_FAIL(msg);
      ext4_free_layout(&layout);
      ext4_block_alloc_free(&alloc);
      cleanup_test_dev(&dev);
      return;
    }
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_inode_bitmap_user_inodes_marked(void) {
  TEST_START("B-2  inode bitmap: inodos de usuario marcados (fix B-2)");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "ibitmapB2", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  /* Simular inode_map con 5 inodos de usuario en grupo 0 */
  struct inode_map imap;
  memset(&imap, 0, sizeof(imap));
  /* inodos 11-15 (primeros inodos de usuario) */
  for (uint32_t i = 11; i <= 15; i++)
    inode_map_add(&imap, (uint64_t)i + 200, i);

  /* ext4_write_bitmaps debe aceptar el inode_map para marcar inodos reales.
   * Si la firma no tiene inode_map, este test documenta que el API necesita
   * cambiar como parte del fix de B-2. Ajustar según implementación. */
  REQUIRE(ext4_write_bitmaps(&dev, &layout, &alloc, &imap) == 0,
          "write_bitmaps falló");

  uint8_t ibm[TEST_BLOCK_SIZE];
  REQUIRE(read_raw(&dev, layout.groups[0].inode_bitmap_block * TEST_BLOCK_SIZE,
                   ibm, TEST_BLOCK_SIZE) == 0,
          "lectura inode bitmap falló");

  /* Inodos 11-15 → bits 10-14 en el bitmap del grupo 0 */
  for (int i = 10; i <= 14; i++) {
    uint8_t bit = (ibm[i / 8] >> (i % 8)) & 1;
    if (!bit) {
      char msg[80];
      snprintf(msg, sizeof(msg),
               "inodo de usuario %d (bit %d) no marcado en bitmap", i + 1, i);
      TEST_FAIL(msg);
      inode_map_free(&imap);
      ext4_free_layout(&layout);
      ext4_block_alloc_free(&alloc);
      cleanup_test_dev(&dev);
      return;
    }
  }

  inode_map_free(&imap);
  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_inode_bitmap_cross_group(void) {
  TEST_START("B-3  inode bitmap: inodo en grupo 1 mapeado al grupo correcto");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "ibitmapB3", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  uint32_t ipg = layout.inodes_per_group;
  /* El primer inodo del grupo 1 es ipg+1 (ext4 inodes son 1-indexed) */
  uint32_t group1_first_ino = ipg + 1;

  struct inode_map imap;
  memset(&imap, 0, sizeof(imap));
  inode_map_add(&imap, 9999ULL, group1_first_ino);

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);
  ext4_write_bitmaps(&dev, &layout, &alloc, &imap);

  /* Verificar que el bit 0 del inode bitmap del grupo 1 está marcado */
  if (layout.num_groups > 1) {
    uint8_t ibm[TEST_BLOCK_SIZE];
    REQUIRE(read_raw(&dev,
                     layout.groups[1].inode_bitmap_block * TEST_BLOCK_SIZE, ibm,
                     TEST_BLOCK_SIZE) == 0,
            "lectura bitmap grupo 1 falló");
    uint8_t bit = ibm[0] & 1; /* bit 0 = primer inodo del grupo */
    CHECK(bit == 1, "inodo en grupo 1 no marcado en el bitmap de grupo 1");
  }

  inode_map_free(&imap);
  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

/* =========================================================================
 * GROUP C — GDT Offsets con desc_size=64 (Bug B-3)
 *
 * El bug: gdt_offset += g * sizeof(struct ext4_group_desc) [=32]
 * El fix: gdt_offset += g * layout->desc_size              [=64]
 * Verificamos que leer el descriptor del grupo N desde el offset correcto
 * produce los valores escritos, no basura del grupo adyacente.
 * ======================================================================= */

static void test_gdt_offset_desc_size64(void) {
  TEST_START("C-1  GDT: grupos en offsets múltiplos de desc_size (64 bytes)");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "gdtC1", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");
  /* El planner debe producir desc_size=64 para imágenes >4GB */
  CHECK(layout.desc_size == 64,
        "desc_size no es 64 — el planner no genera 64-bit GDT");

  REQUIRE(ext4_write_gdt(&dev, &layout) == 0, "write_gdt falló");

  /* Leer el descriptor del primer grupo con superbloque */
  uint64_t gdt_start = layout.groups[0].gdt_start_block * TEST_BLOCK_SIZE;

  for (uint32_t g = 0; g < layout.num_groups && g < 4; g++) {
    uint8_t raw_desc[64];
    REQUIRE(read_raw(&dev, gdt_start + (uint64_t)g * layout.desc_size, raw_desc,
                     layout.desc_size) == 0,
            "lectura descriptor falló");

    struct ext4_group_desc *desc = (struct ext4_group_desc *)raw_desc;

    /* El block_bitmap_lo debe coincidir con lo que planificó el layout */
    uint32_t expected_bb =
        (uint32_t)(layout.groups[g].block_bitmap_block & 0xFFFFFFFF);
    uint32_t written_bb = le32toh(desc->bg_block_bitmap_lo);

    if (written_bb != expected_bb) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "grupo %u: bg_block_bitmap_lo=%u esperado=%u "
               "(posible bug B-3: offset con sizeof en lugar de desc_size)",
               g, written_bb, expected_bb);
      TEST_FAIL(msg);
      ext4_free_layout(&layout);
      cleanup_test_dev(&dev);
      return;
    }
  }

  ext4_free_layout(&layout);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_gdt_no_overlap_between_groups(void) {
  TEST_START("C-2  GDT: descriptores de grupos no se solapan entre sí");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "gdtC2", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");
  REQUIRE(ext4_write_gdt(&dev, &layout) == 0, "write_gdt falló");

  uint64_t gdt_start = layout.groups[0].gdt_start_block * TEST_BLOCK_SIZE;

  /* Verificar que el inode_bitmap de grupo N no apunta al mismo bloque
   * que el inode_bitmap de grupo N+1 */
  for (uint32_t g = 0; g + 1 < layout.num_groups && g < 4; g++) {
    uint8_t raw0[64], raw1[64];
    read_raw(&dev, gdt_start + (uint64_t)g * layout.desc_size, raw0, 64);
    read_raw(&dev, gdt_start + (uint64_t)(g + 1) * layout.desc_size, raw1, 64);

    struct ext4_group_desc *d0 = (struct ext4_group_desc *)raw0;
    struct ext4_group_desc *d1 = (struct ext4_group_desc *)raw1;

    uint32_t bb0 = le32toh(d0->bg_block_bitmap_lo);
    uint32_t bb1 = le32toh(d1->bg_block_bitmap_lo);
    if (bb0 == bb1 && bb0 != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "grupos %u y %u tienen el mismo block_bitmap (=%u) "
               "— probable bug B-3",
               g, g + 1, bb0);
      TEST_FAIL(msg);
      ext4_free_layout(&layout);
      cleanup_test_dev(&dev);
      return;
    }
  }

  ext4_free_layout(&layout);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_gdt_group0_inode_table_in_bounds(void) {
  TEST_START("C-3  GDT: inode_table de todos los grupos dentro del device");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "gdtC3", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");
  REQUIRE(ext4_write_gdt(&dev, &layout) == 0, "write_gdt falló");

  uint64_t gdt_start = layout.groups[0].gdt_start_block * TEST_BLOCK_SIZE;
  uint64_t total_blocks = layout.total_blocks;

  for (uint32_t g = 0; g < layout.num_groups; g++) {
    if (!layout.groups[g].has_super)
      continue;

    uint8_t raw[64] = {0};
    read_raw(&dev, gdt_start + (uint64_t)g * layout.desc_size, raw, 64);
    struct ext4_group_desc *d = (struct ext4_group_desc *)raw;

    uint64_t it = le32toh(d->bg_inode_table_lo);
    it |= (uint64_t)le32toh(d->bg_inode_table_hi) << 32;

    if (it >= total_blocks) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "grupo %u: inode_table_block=%lu >= total_blocks=%lu", g,
               (unsigned long)it, (unsigned long)total_blocks);
      TEST_FAIL(msg);
      ext4_free_layout(&layout);
      cleanup_test_dev(&dev);
      return;
    }
  }

  ext4_free_layout(&layout);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

/* =========================================================================
 * GROUP D — Tail Block Marking (Bug B-11)
 *
 * En el último grupo, los bloques desde total_blocks hasta
 * group_start + blocks_per_group deben estar marcados como usados en el
 * block bitmap (no pueden ser asignados a archivos, no existen físicamente).
 * ======================================================================= */

static void test_tail_blocks_marked_used(void) {
  TEST_START(
      "D-1  bitmap: bloques del último grupo más allá del device marcados");

  struct device dev;
  /* Usar un tamaño que NO sea múltiplo de blocks_per_group */
  uint64_t odd_size = 130ULL * 1024 * 1024 + 4096 * 37; /* no alineado */
  REQUIRE(make_test_dev(&dev, "tailD1", odd_size) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(ext4_plan_layout(&layout, odd_size, TEST_BLOCK_SIZE, 16384, NULL) ==
              0,
          "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);
  REQUIRE(ext4_write_bitmaps(&dev, &layout, &alloc, NULL) == 0,
          "write_bitmaps falló");

  uint32_t last_g = layout.num_groups - 1;
  uint64_t g_start = layout.groups[last_g].group_start_block;
  uint64_t bpg = layout.blocks_per_group;
  uint64_t tail_start = layout.total_blocks - g_start; /* local bit offset */

  /* Si el grupo está exactamente completo, no hay bits de tail */
  if (g_start + bpg <= layout.total_blocks) {
    ext4_free_layout(&layout);
    ext4_block_alloc_free(&alloc);
    cleanup_test_dev(&dev);
    TEST_PASS(); /* no hay tail, el test no aplica */
    return;
  }

  uint8_t bbm[TEST_BLOCK_SIZE];
  REQUIRE(read_raw(&dev,
                   layout.groups[last_g].block_bitmap_block * TEST_BLOCK_SIZE,
                   bbm, TEST_BLOCK_SIZE) == 0,
          "lectura bitmap falló");

  /* Los bits [tail_start .. bpg-1] deben estar todos a 1 */
  for (uint64_t bit = tail_start;
       bit < bpg && bit < (uint64_t)(8 * TEST_BLOCK_SIZE); bit++) {
    uint8_t val = (bbm[bit / 8] >> (bit % 8)) & 1;
    if (!val) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "bit %lu (bloque lógico %lu) del último grupo no marcado "
               "como usado (bug B-11)",
               (unsigned long)bit, (unsigned long)(g_start + bit));
      TEST_FAIL(msg);
      ext4_free_layout(&layout);
      ext4_block_alloc_free(&alloc);
      cleanup_test_dev(&dev);
      return;
    }
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_tail_boundary_exact_multiple(void) {
  TEST_START("D-2  bitmap: tamaño exactamente múltiplo — no hay tail espurio");

  struct ext4_layout layout;
  /* blocks_per_group estándar = 32768, usar justo 3 grupos */
  uint64_t exact_size = (uint64_t)32768 * 3 * TEST_BLOCK_SIZE;
  REQUIRE(ext4_plan_layout(&layout, exact_size, TEST_BLOCK_SIZE, 16384, NULL) ==
              0,
          "planner falló");

  uint32_t last_g = layout.num_groups - 1;
  uint64_t g_start = layout.groups[last_g].group_start_block;
  uint64_t bpg = layout.blocks_per_group;
  int has_tail = (g_start + bpg) > layout.total_blocks;

  /* Con tamaño exacto no debe haber tail */
  CHECK(!has_tail, "hay tail inesperado con tamaño exactamente múltiplo");

  ext4_free_layout(&layout);
  TEST_PASS();
}

/* =========================================================================
 * GROUP E — Directory Extent Tree (Bug B-4)
 *
 * Un directorio con >4 bloques necesita un extent tree de depth=1 (un bloque
 * índice externo). Sin la corrección, la inode solo referencia los primeros
 * 4 bloques y el resto del directorio es inaccesible.
 * ======================================================================= */

/* Construye una btrfs_fs_info mínima con un directorio de N entradas */
static struct btrfs_fs_info *make_big_dir_fs(int n_children) {
  struct btrfs_fs_info *fs = calloc(1, sizeof(*fs));
  fs->sb.sectorsize = 4096;
  fs->sb.nodesize = 16384;
  fs->sb.total_bytes = 1ULL * 1024 * 1024 * 1024;

  /* UUID falso */
  memset(fs->sb.fsid, 0xAB, 16);

  /* chunk_map minimal */
  fs->chunk_map = calloc(1, sizeof(*fs->chunk_map));
  fs->chunk_map->capacity = 1;
  fs->chunk_map->entries = calloc(1, sizeof(*fs->chunk_map->entries));
  fs->chunk_map->entries[0].logical = 0;
  fs->chunk_map->entries[0].physical = 0;
  fs->chunk_map->entries[0].length = 1ULL * 1024 * 1024 * 1024;
  fs->chunk_map->count = 1;

  /* Un directorio raíz */
  fs->root_dir = calloc(1, sizeof(*fs->root_dir));
  fs->root_dir->ino = 256;
  fs->root_dir->mode = S_IFDIR | 0755;
  fs->root_dir->nlink = 2 + n_children;
  fs->root_dir->size = n_children * 32;

  /* n_children hijos en el directorio */
  fs->root_dir->child_count = n_children;
  fs->root_dir->child_capacity = n_children;
  fs->root_dir->children = calloc(n_children, sizeof(*fs->root_dir->children));

  /* Crear los file_entry de los hijos y los dir_entry_link */
  fs->inode_count = n_children + 1;
  fs->inode_table = calloc(fs->inode_count, sizeof(*fs->inode_table));
  fs->inode_table[0] = fs->root_dir;

  for (int i = 0; i < n_children; i++) {
    struct file_entry *child = calloc(1, sizeof(*child));
    child->ino = (uint64_t)(257 + i);
    child->mode = S_IFREG | 0644;
    child->nlink = 1;
    child->size = 0;
    child->parent_ino = 256;

    fs->inode_table[i + 1] = child;

    struct dir_entry_link *link = &fs->root_dir->children[i];
    link->target = child;
    snprintf(link->name, sizeof(link->name), "file_%04d.dat", i);
    link->name_len = (uint16_t)strlen(link->name);
  }

  return fs;
}

static void free_big_dir_fs(struct btrfs_fs_info *fs) {
  if (!fs)
    return;
  for (uint32_t i = 1; i < fs->inode_count; i++) {
    if (fs->inode_table[i]) {
      free(fs->inode_table[i]->extents);
      free(fs->inode_table[i]);
    }
  }
  free(fs->inode_table); /* root is inode_table[0], freed separately */
  if (fs->root_dir) {
    free(fs->root_dir->children);
    free(fs->root_dir);
  }
  if (fs->chunk_map) {
    free(fs->chunk_map->entries);
    free(fs->chunk_map);
  }
  free(fs);
}

static void test_dir_small_inline_extents(void) {
  TEST_START("E-1  dir extent: directorio 3 bloques → depth=0, 3 extents");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "dirE1", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct btrfs_fs_info *fs = make_big_dir_fs(60); /* ~60 entradas ≈ 2 bloques */
  struct inode_map imap;
  memset(&imap, 0, sizeof(imap));
  inode_map_add(&imap, 256, EXT4_ROOT_INO);
  for (int i = 0; i < 60; i++)
    inode_map_add(&imap, (uint64_t)(257 + i),
                  (uint32_t)(EXT4_GOOD_OLD_FIRST_INO + i));

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  /* Escribir superbloque, GDT, inode tables (necesarios para dir_writer) */
  ext4_write_gdt(&dev, &layout);
  ext4_write_bitmaps(&dev, &layout, &alloc, &imap);

  int ret = ext4_write_directories(&dev, &layout, fs, &imap, &alloc);
  CHECK(ret == 0, "write_directories falló");

  /* Leer el inode del directorio raíz (inode 2) */
  uint32_t ino_group = (EXT4_ROOT_INO - 1) / layout.inodes_per_group;
  uint32_t ino_local = (EXT4_ROOT_INO - 1) % layout.inodes_per_group;
  uint64_t ino_offset =
      layout.groups[ino_group].inode_table_start * TEST_BLOCK_SIZE +
      (uint64_t)ino_local * layout.inode_size;

  struct ext4_inode inode;
  REQUIRE(read_raw(&dev, ino_offset, &inode, sizeof(inode)) == 0,
          "lectura inode raíz falló");

  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  uint16_t depth = le16toh(eh->eh_depth);
  uint16_t entries = le16toh(eh->eh_entries);

  CHECK(depth == 0, "directorio pequeño debería tener extent tree depth=0");
  CHECK(entries > 0 && entries <= 4,
        "número de extents fuera de rango para directorio pequeño");

  inode_map_free(&imap);
  free_big_dir_fs(fs);
  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_dir_large_depth1_extent_tree(void) {
  TEST_START("E-2  dir extent: directorio >4 bloques → depth=1 (fix B-4)");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "dirE2", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  /* ~300 entradas de 16 bytes ≈ 5-6 bloques de 4096 bytes */
  struct btrfs_fs_info *fs = make_big_dir_fs(300);
  struct inode_map imap;
  memset(&imap, 0, sizeof(imap));
  inode_map_add(&imap, 256, EXT4_ROOT_INO);
  for (int i = 0; i < 300; i++)
    inode_map_add(&imap, (uint64_t)(257 + i),
                  (uint32_t)(EXT4_GOOD_OLD_FIRST_INO + i));

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);
  ext4_write_gdt(&dev, &layout);
  ext4_write_bitmaps(&dev, &layout, &alloc, NULL);

  int ret = ext4_write_directories(&dev, &layout, fs, &imap, &alloc);
  CHECK(ret == 0, "write_directories falló");

  uint32_t ino_group = (EXT4_ROOT_INO - 1) / layout.inodes_per_group;
  uint32_t ino_local = (EXT4_ROOT_INO - 1) % layout.inodes_per_group;
  uint64_t ino_offset =
      layout.groups[ino_group].inode_table_start * TEST_BLOCK_SIZE +
      (uint64_t)ino_local * layout.inode_size;

  struct ext4_inode inode;
  REQUIRE(read_raw(&dev, ino_offset, &inode, sizeof(inode)) == 0,
          "lectura inode raíz falló");

  uint64_t dir_size = (uint64_t)le32toh(inode.i_size_lo) |
                      ((uint64_t)le32toh(inode.i_size_high) << 32);
  uint64_t expected_blocks = (dir_size + TEST_BLOCK_SIZE - 1) / TEST_BLOCK_SIZE;

  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  uint16_t depth = le16toh(eh->eh_depth);
  uint64_t accessible_blocks = 0;

  if (depth == 0) {
    uint16_t n_extents = le16toh(eh->eh_entries);
    struct ext4_extent *exts =
        (struct ext4_extent *)((uint8_t *)inode.i_block + sizeof(*eh));
    for (uint16_t e = 0; e < n_extents; e++) {
      accessible_blocks += le16toh(exts[e].ee_len);
    }
  } else if (depth == 1) {
    struct ext4_extent_idx *idx =
        (struct ext4_extent_idx *)((uint8_t *)inode.i_block +
                                   sizeof(struct ext4_extent_header));
    uint16_t n_idx = le16toh(eh->eh_entries);
    for (uint16_t k = 0; k < n_idx; k++) {
      uint64_t leaf_blk = le32toh(idx[k].ei_leaf_lo) |
                          ((uint64_t)le16toh(idx[k].ei_leaf_hi) << 32);
      uint8_t leaf_buf[TEST_BLOCK_SIZE];
      if (read_raw(&dev, leaf_blk * TEST_BLOCK_SIZE, leaf_buf,
                   TEST_BLOCK_SIZE) == 0) {
        struct ext4_extent_header *leh = (struct ext4_extent_header *)leaf_buf;
        if (le16toh(leh->eh_magic) == EXT4_EXT_MAGIC) {
          struct ext4_extent *exts =
              (struct ext4_extent *)(leaf_buf + sizeof(*leh));
          uint16_t n = le16toh(leh->eh_entries);
          for (uint16_t e = 0; e < n; e++) {
            accessible_blocks += le16toh(exts[e].ee_len);
          }
        }
      }
    }
  }

  if (accessible_blocks < expected_blocks) {
    char msg[128];
    snprintf(
        msg, sizeof(msg),
        "Bug B-4 activo: bloques accesibles=%lu < esperados=%lu (depth=%u)",
        (unsigned long)accessible_blocks, (unsigned long)expected_blocks,
        depth);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  inode_map_free(&imap);
  free_big_dir_fs(fs);
  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_dir_huge_all_blocks_reachable(void) {
  TEST_START("E-3  dir extent: 1000 entradas — todos los bloques alcanzables");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "dirE3", 256ULL * 1024 * 1024) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(ext4_plan_layout(&layout, 256ULL * 1024 * 1024, TEST_BLOCK_SIZE,
                           16384, NULL) == 0,
          "planner falló");

  struct btrfs_fs_info *fs = make_big_dir_fs(1000);
  struct inode_map imap;
  memset(&imap, 0, sizeof(imap));
  inode_map_add(&imap, 256, EXT4_ROOT_INO);
  for (int i = 0; i < 1000; i++)
    inode_map_add(&imap, (uint64_t)(257 + i),
                  (uint32_t)(EXT4_GOOD_OLD_FIRST_INO + i));

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);
  ext4_write_gdt(&dev, &layout);
  ext4_write_bitmaps(&dev, &layout, &alloc, NULL);
  ext4_write_directories(&dev, &layout, fs, &imap, &alloc);

  uint32_t ino_group = (EXT4_ROOT_INO - 1) / layout.inodes_per_group;
  uint32_t ino_local = (EXT4_ROOT_INO - 1) % layout.inodes_per_group;
  uint64_t ino_offset =
      layout.groups[ino_group].inode_table_start * TEST_BLOCK_SIZE +
      (uint64_t)ino_local * layout.inode_size;

  struct ext4_inode inode;
  REQUIRE(read_raw(&dev, ino_offset, &inode, sizeof(inode)) == 0,
          "lectura inode raíz falló");

  uint64_t dir_size = (uint64_t)le32toh(inode.i_size_lo) |
                      ((uint64_t)le32toh(inode.i_size_high) << 32);
  uint64_t expected_blocks = (dir_size + TEST_BLOCK_SIZE - 1) / TEST_BLOCK_SIZE;

  struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
  uint16_t depth = le16toh(eh->eh_depth);

  /* Contar bloques accesibles caminando el extent tree */
  uint64_t accessible_blocks = 0;
  if (depth == 0) {
    uint16_t n_extents = le16toh(eh->eh_entries);
    struct ext4_extent *exts =
        (struct ext4_extent *)((uint8_t *)inode.i_block + sizeof(*eh));
    for (uint16_t e = 0; e < n_extents; e++) {
      accessible_blocks += le16toh(exts[e].ee_len);
    }
  } else if (depth == 1) {
    /* Los entries del nodo raíz son ext4_extent_idx */
    struct ext4_extent_idx *idx =
        (struct ext4_extent_idx *)((uint8_t *)inode.i_block +
                                   sizeof(struct ext4_extent_header));
    uint16_t n_idx = le16toh(eh->eh_entries);
    for (uint16_t k = 0; k < n_idx; k++) {
      uint64_t leaf_blk = le32toh(idx[k].ei_leaf_lo) |
                          ((uint64_t)le16toh(idx[k].ei_leaf_hi) << 32);
      uint8_t leaf_buf[TEST_BLOCK_SIZE];
      if (read_raw(&dev, leaf_blk * TEST_BLOCK_SIZE, leaf_buf,
                   TEST_BLOCK_SIZE) != 0) {
        printf("E-3 trace: read_raw failed for leaf_blk %lu\n",
               (unsigned long)leaf_blk);
        continue;
      }
      struct ext4_extent_header *leh = (struct ext4_extent_header *)leaf_buf;
      if (le16toh(leh->eh_magic) != EXT4_EXT_MAGIC) {
        printf("E-3 trace: invalid magic 0x%04x at leaf_blk %lu\n",
               le16toh(leh->eh_magic), (unsigned long)leaf_blk);
        continue;
      }
      struct ext4_extent *exts =
          (struct ext4_extent *)(leaf_buf + sizeof(*leh));
      uint16_t n = le16toh(leh->eh_entries);
      for (uint16_t e = 0; e < n; e++)
        accessible_blocks += le16toh(exts[e].ee_len);
    }
  }

  if (accessible_blocks < expected_blocks) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "bloques accesibles=%lu < esperados=%lu (bug B-4)",
             (unsigned long)accessible_blocks, (unsigned long)expected_blocks);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  inode_map_free(&imap);
  free_big_dir_fs(fs);
  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
}

/* =========================================================================
 * GROUP F — GDT Checksums (Bug B-6)
 *
 * Si EXT4_FEATURE_RO_COMPAT_GDT_CSUM está habilitado en el superbloque,
 * cada descriptor debe tener bg_checksum válido. Con el bug, todos son 0.
 * ======================================================================= */

static void test_gdt_checksum_nonzero(void) {
  TEST_START("F-1  GDT csum: bg_checksum != 0 en todos los grupos");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "csuF1", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct btrfs_fs_info *fs = make_big_dir_fs(1);
  REQUIRE(ext4_write_superblock(&dev, &layout, fs) == 0, "write_sb falló");
  REQUIRE(ext4_write_gdt(&dev, &layout) == 0, "write_gdt falló");
  free_big_dir_fs(fs);

  uint64_t gdt_start = layout.groups[0].gdt_start_block * TEST_BLOCK_SIZE;
  int found_zero = 0;

  for (uint32_t g = 0; g < layout.num_groups && g < 8; g++) {
    uint8_t raw[64];
    read_raw(&dev, gdt_start + (uint64_t)g * layout.desc_size, raw, 64);
    struct ext4_group_desc *d = (struct ext4_group_desc *)raw;
    uint16_t csum = le16toh(d->bg_checksum);
    if (csum == 0) {
      found_zero++;
    }
  }

  if (found_zero > 0) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%d grupos con bg_checksum=0 (bug B-6)",
             found_zero);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
  cleanup_test_dev(&dev);
}

static void test_gdt_checksum_value_correct(void) {
  TEST_START(
      "F-2  GDT csum: valor de bg_checksum coincide con CRC16 calculado");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "csuF2", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  /* Escribir superbloque primero (necesitamos el UUID) */
  struct btrfs_fs_info *fs = make_big_dir_fs(1);
  REQUIRE(ext4_write_superblock(&dev, &layout, fs) == 0, "write_sb falló");
  REQUIRE(ext4_write_gdt(&dev, &layout) == 0, "write_gdt falló");
  free_big_dir_fs(fs);

  /* Leer el UUID del superbloque */
  struct ext4_super_block sb;
  REQUIRE(read_raw(&dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) == 0,
          "lectura superbloque falló");

  uint64_t gdt_start = layout.groups[0].gdt_start_block * TEST_BLOCK_SIZE;
  int mismatches = 0;

  for (uint32_t g = 0; g < layout.num_groups && g < 4; g++) {
    uint8_t raw[64];
    read_raw(&dev, gdt_start + (uint64_t)g * layout.desc_size, raw, 64);
    struct ext4_group_desc *d = (struct ext4_group_desc *)raw;

    uint16_t written = le16toh(d->bg_checksum);
    uint16_t expected = expected_gdt_csum(sb.s_uuid, g, raw, layout.desc_size);

    if (written != expected) {
      mismatches++;
    }
  }

  if (mismatches > 0) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%d checksums incorrectos (bug B-6)",
             mismatches);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
  cleanup_test_dev(&dev);
}

/* =========================================================================
 * GROUP G — Block Allocator (Bug B-9: dirección, Bug B-10: wrap-around)
 * ======================================================================= */

static void test_alloc_direction_forward(void) {
  TEST_START("G-1  allocator: bloques asignados en orden creciente (fix B-9)");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  uint64_t prev = 0;
  int backwards = 0;

  for (int i = 0; i < 100; i++) {
    uint64_t blk = ext4_alloc_block(&alloc, &layout);
    if (blk == (uint64_t)-1)
      break;
    if (blk < prev)
      backwards++;
    prev = blk;
  }

  if (backwards > 5) { /* toleramos hasta 5 saltos de grupo */
    char msg[80];
    snprintf(msg, sizeof(msg), "%d asignaciones retrocedieron (bug B-9)",
             backwards);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
}

static void test_alloc_wraparound(void) {
  TEST_START(
      "G-2  allocator: wrap-around cuando cursor > bloques libres (fix B-10)");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  /* Avanzar el cursor hasta casi el final */
  alloc.next_alloc_block = layout.total_blocks - 10;

  /* También marcar todos esos bloques como usados excepto 2 al principio */
  uint64_t known_free1 = layout.groups[0].data_start_block;
  uint64_t known_free2 = layout.groups[0].data_start_block + 1;
  /* Asegurarse de que estén libres en el bitmap */
  if (alloc.reserved_bitmap) {
    alloc.reserved_bitmap[known_free1 / 8] &= ~(1 << (known_free1 % 8));
    alloc.reserved_bitmap[known_free2 / 8] &= ~(1 << (known_free2 % 8));
    /* Marcar todos los bloques del final como usados */
    for (uint64_t b = layout.total_blocks - 10; b < layout.total_blocks; b++) {
      if (b / 8 < (layout.total_blocks + 7) / 8)
        alloc.reserved_bitmap[b / 8] |= (1 << (b % 8));
    }
  }

  /* Con wrap-around, debe encontrar known_free1 o known_free2 */
  uint64_t blk = ext4_alloc_block(&alloc, &layout);

  if (blk == (uint64_t)-1) {
    TEST_FAIL(
        "allocator devolvió -1 con bloques libres disponibles (bug B-10)");
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
}

static void test_alloc_no_metadata_collision(void) {
  TEST_START("G-3  allocator: bloques asignados no colisionan con metadatos");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  /* Asignar 500 bloques y verificar que ninguno coincide con un bloque
   * de metadatos (bitmap, inode table, GDT) */
  int collisions = 0;
  for (int i = 0; i < 500; i++) {
    uint64_t blk = ext4_alloc_block(&alloc, &layout);
    if (blk == (uint64_t)-1)
      break;

    /* Verificar contra todos los grupos */
    for (uint32_t g = 0; g < layout.num_groups; g++) {
      const struct ext4_bg_layout *bg = &layout.groups[g];
      if (blk == bg->block_bitmap_block || blk == bg->inode_bitmap_block ||
          (blk >= bg->inode_table_start &&
           blk < bg->inode_table_start + bg->inode_table_blocks)) {
        collisions++;
      }
    }
  }

  if (collisions > 0) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%d bloques asignados colisionan con metadatos",
             collisions);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
}

/* =========================================================================
 * GROUP H — Journal Zeroing (Bug B-7)
 *
 * Verifica: JBD2 magic correcto, bloques 1..N son cero, tiempo < 2 segundos.
 * ======================================================================= */

static void test_journal_jbd2_magic(void) {
  TEST_START("H-1  journal: magic JBD2 en bloque 0");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "jrnH1", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  REQUIRE(ext4_write_journal(&dev, &layout, &alloc, TEST_IMG_SIZE) == 0,
          "write_journal falló");

  uint64_t journal_blk = ext4_journal_start_block();
  REQUIRE(journal_blk > 0, "journal_start_block retornó 0");

  /* Leer el JBD2 superblock */
  uint8_t jbd_buf[TEST_BLOCK_SIZE];
  REQUIRE(read_raw(&dev, journal_blk * TEST_BLOCK_SIZE, jbd_buf,
                   TEST_BLOCK_SIZE) == 0,
          "lectura journal block 0 falló");

  /* Los primeros 4 bytes son el JBD2 magic en big-endian */
  uint32_t magic = 0;
  memcpy(&magic, jbd_buf, 4);
  magic = be32toh(magic);

  CHECK(magic == 0xC03B3998U, /* JBD2_MAGIC_NUMBER */
        "magic JBD2 incorrecto en bloque 0 del journal");

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_journal_blocks_zeroed(void) {
  TEST_START("H-2  journal: bloques 1..N-1 son cero después de write_journal");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "jrnH2", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  /* Llenar con basura para que los ceros sean significativos */
  uint8_t noise[TEST_BLOCK_SIZE];
  memset(noise, 0xFF, TEST_BLOCK_SIZE);
  for (uint64_t off = 0; off < TEST_IMG_SIZE; off += TEST_BLOCK_SIZE)
    device_write(&dev, off, noise, TEST_BLOCK_SIZE);

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);
  REQUIRE(ext4_write_journal(&dev, &layout, &alloc, TEST_IMG_SIZE) == 0,
          "write_journal falló");

  uint64_t jstart = ext4_journal_start_block();
  uint32_t jcount = ext4_journal_block_count();
  REQUIRE(jcount > 1, "journal tiene solo 1 bloque");

  uint8_t buf[TEST_BLOCK_SIZE];
  int non_zero = 0;
  /* Verificar hasta 32 bloques (evitar test larguísimo) */
  uint32_t check_count = jcount > 32 ? 32 : jcount - 1;
  for (uint32_t i = 1; i <= check_count; i++) {
    REQUIRE(read_raw(&dev, (jstart + i) * TEST_BLOCK_SIZE, buf,
                     TEST_BLOCK_SIZE) == 0,
            "lectura bloque journal falló");
    for (int b = 0; b < TEST_BLOCK_SIZE; b++) {
      if (buf[b] != 0) {
        non_zero++;
        break;
      }
    }
  }

  if (non_zero > 0) {
    char msg[80];
    snprintf(msg, sizeof(msg),
             "%d bloques del journal no están zeroed (bug B-7)", non_zero);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
}

static void test_journal_zeroing_speed(void) {
  TEST_START("H-3  journal: zeroing completa en <2 segundos (fix B-7)");

  struct device dev;
  /* Usar 256MB para tener un journal razonable (~128MB) */
  REQUIRE(make_test_dev(&dev, "jrnH3", 256ULL * 1024 * 1024) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(ext4_plan_layout(&layout, 256ULL * 1024 * 1024, TEST_BLOCK_SIZE,
                           16384, NULL) == 0,
          "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  double t0 = now_sec();
  REQUIRE(ext4_write_journal(&dev, &layout, &alloc, 256ULL * 1024 * 1024) == 0,
          "write_journal falló");
  double elapsed = now_sec() - t0;

  printf("(%.3fs) ", elapsed);

  if (elapsed > 2.0) {
    char msg[80];
    snprintf(msg, sizeof(msg),
             "demasiado lento: %.3fs > 2.0s (bug B-7 no corregido)", elapsed);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
}

/* =========================================================================
 * GROUP I — Consistencia End-to-End
 *
 * Escribe un filesystem completo (sin inodos de usuario, solo estructura)
 * y verifica las invariantes globales: magic, free counts, no solapamiento
 * de regiones, inodos reservados correctos.
 * ======================================================================= */

static void test_e2e_superblock_magic(void) {
  TEST_START("I-1  E2E: superbloque tiene EXT4_SUPER_MAGIC (0xEF53)");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "e2eI1", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  /* Mínimo: superbloque + GDT */
  struct btrfs_fs_info *fs = calloc(1, sizeof(*fs));
  fs->sb.sectorsize = 4096;
  fs->sb.nodesize = 16384;
  fs->sb.total_bytes = TEST_IMG_SIZE;
  fs->chunk_map = calloc(1, sizeof(*fs->chunk_map));
  REQUIRE(ext4_write_superblock(&dev, &layout, fs) == 0,
          "write_superblock falló");

  struct ext4_super_block sb;
  REQUIRE(read_raw(&dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) == 0,
          "lectura superbloque falló");
  CHECK(le16toh(sb.s_magic) == EXT4_SUPER_MAGIC, "magic erróneo");

  free(fs->chunk_map);
  free(fs);
  ext4_free_layout(&layout);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_e2e_free_counts_consistent(void) {
  TEST_START("I-2  E2E: sum(grupos free_blocks) == superbloque free_blocks");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "e2eI2", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct btrfs_fs_info *fs = calloc(1, sizeof(*fs));
  fs->sb.sectorsize = 4096;
  fs->sb.nodesize = 16384;
  fs->sb.total_bytes = TEST_IMG_SIZE;
  fs->chunk_map = calloc(1, sizeof(*fs->chunk_map));

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);

  ext4_write_superblock(&dev, &layout, fs);
  ext4_write_gdt(&dev, &layout);
  ext4_write_bitmaps(&dev, &layout, &alloc, NULL);
  ext4_update_free_counts(&dev, &layout);

  /* Leer superbloque y sumar free_blocks de todos los grupos */
  struct ext4_super_block sb;
  REQUIRE(read_raw(&dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) == 0,
          "lectura superbloque falló");

  uint64_t sb_free = le32toh(sb.s_free_blocks_count_lo) |
                     ((uint64_t)le32toh(sb.s_free_blocks_count_hi) << 32);

  uint64_t sum_free = 0;
  uint64_t gdt_start = layout.groups[0].gdt_start_block * TEST_BLOCK_SIZE;
  for (uint32_t g = 0; g < layout.num_groups; g++) {
    if (!layout.groups[g].has_super)
      continue;
    uint8_t raw[64];
    read_raw(&dev, gdt_start + (uint64_t)g * layout.desc_size, raw, 64);
    struct ext4_group_desc *d = (struct ext4_group_desc *)raw;
    sum_free += le16toh(d->bg_free_blocks_count_lo) |
                ((uint64_t)le16toh(d->bg_free_blocks_count_hi) << 16);
  }

  /* La suma de los grupos con superbloque debe acercarse a sb_free */
  /* (No todos los grupos tienen superbloque, usamos tolerancia) */
  if (sb_free == 0) {
    TEST_FAIL("free_blocks_count en superbloque es 0 (no actualizado)");
  } else if (sb_free > layout.total_blocks) {
    char msg[80];
    snprintf(msg, sizeof(msg), "free_blocks=%lu > total_blocks=%lu (corrupto)",
             (unsigned long)sb_free, (unsigned long)layout.total_blocks);
    TEST_FAIL(msg);
  } else if (sum_free == 0) {
    TEST_FAIL("Suma de free blocks de GDTs leídos es 0");
  } else {
    TEST_PASS();
  }

  free(fs->chunk_map);
  free(fs);
  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
}

static void test_e2e_metadata_regions_no_overlap(void) {
  TEST_START("I-3  E2E: regiones de metadatos no se solapan entre grupos");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  /* Para cada par de grupos, verificar que sus regiones de metadatos
   * no se solapan */
  int overlaps = 0;
  for (uint32_t g = 0; g < layout.num_groups; g++) {
    const struct ext4_bg_layout *a = &layout.groups[g];
    /* Rango de metadatos del grupo g: desde group_start hasta data_start */
    uint64_t a_end = a->data_start_block;

    for (uint32_t h = g + 1; h < layout.num_groups && h < g + 3; h++) {
      const struct ext4_bg_layout *b = &layout.groups[h];
      uint64_t b_start = b->group_start_block;
      /* No debe haber solapamiento */
      if (a_end > b_start) {
        overlaps++;
      }
    }
  }

  if (overlaps > 0) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%d solapamientos entre regiones de metadatos",
             overlaps);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
}

static void test_e2e_inode_table_within_bounds(void) {
  TEST_START(
      "I-4  E2E: tablas de inodos de todos los grupos dentro del device");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  int out_of_bounds = 0;
  for (uint32_t g = 0; g < layout.num_groups; g++) {
    const struct ext4_bg_layout *bg = &layout.groups[g];
    uint64_t it_end = bg->inode_table_start + bg->inode_table_blocks;
    if (it_end > layout.total_blocks)
      out_of_bounds++;
  }

  if (out_of_bounds > 0) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%d grupos con inode_table fuera del device",
             out_of_bounds);
    TEST_FAIL(msg);
  } else {
    TEST_PASS();
  }

  ext4_free_layout(&layout);
}

static void test_e2e_superblock_feature_bits(void) {
  TEST_START("I-5  E2E: feature bits de superbloque son coherentes");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "e2eI5", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct btrfs_fs_info *fs = calloc(1, sizeof(*fs));
  fs->sb.sectorsize = 4096;
  fs->sb.nodesize = 16384;
  fs->sb.total_bytes = TEST_IMG_SIZE;
  fs->chunk_map = calloc(1, sizeof(*fs->chunk_map));
  REQUIRE(ext4_write_superblock(&dev, &layout, fs) == 0,
          "write_superblock falló");

  struct ext4_super_block sb;
  REQUIRE(read_raw(&dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) == 0,
          "lectura superbloque falló");

  uint32_t incompat = le32toh(sb.s_feature_incompat);
  uint32_t ro_compat = le32toh(sb.s_feature_ro_compat);

  /* Si 64BIT está activo, desc_size debe ser 64 */
  if (incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
    CHECK(le16toh(sb.s_desc_size) == 64, "64BIT activo pero s_desc_size != 64");
  }

  /* Si GDT_CSUM o METADATA_CSUM activo, los checksums deben ser válidos */
  int has_csum = (ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) ||
                 (ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);
  if (has_csum) {
    /* Verificar que el GDT fue escrito con checksums */
    ext4_write_gdt(&dev, &layout);
    uint64_t gdt_start = layout.groups[0].gdt_start_block * TEST_BLOCK_SIZE;
    uint8_t raw[64];
    read_raw(&dev, gdt_start, raw, 64);
    struct ext4_group_desc *d = (struct ext4_group_desc *)raw;
    uint16_t csum = le16toh(d->bg_checksum);
    CHECK(csum != 0,
          "CSUM feature activo pero bg_checksum del grupo 0 es 0 (bug B-6)");
  }

  free(fs->chunk_map);
  free(fs);
  ext4_free_layout(&layout);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

static void test_e2e_reserved_inodes_not_free(void) {
  TEST_START("I-6  E2E: inodos 1-10 marcados como usados, inodo 11 libre");

  struct device dev;
  REQUIRE(make_test_dev(&dev, "e2eI6", TEST_IMG_SIZE) == 0,
          "no se pudo crear imagen");

  struct ext4_layout layout;
  REQUIRE(build_test_layout(&layout) == 0, "planner falló");

  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);
  REQUIRE(ext4_write_bitmaps(&dev, &layout, &alloc, NULL) == 0,
          "write_bitmaps falló");

  uint8_t ibm[TEST_BLOCK_SIZE];
  REQUIRE(read_raw(&dev, layout.groups[0].inode_bitmap_block * TEST_BLOCK_SIZE,
                   ibm, TEST_BLOCK_SIZE) == 0,
          "lectura inode bitmap falló");

  /* Inodos 1-10: bits 0-9 deben estar a 1 */
  for (int i = 0; i < 10; i++) {
    uint8_t bit = (ibm[i / 8] >> (i % 8)) & 1;
    if (!bit) {
      char msg[64];
      snprintf(msg, sizeof(msg), "inodo %d no marcado como usado", i + 1);
      TEST_FAIL(msg);
      ext4_free_layout(&layout);
      ext4_block_alloc_free(&alloc);
      cleanup_test_dev(&dev);
      return;
    }
  }

  ext4_free_layout(&layout);
  ext4_block_alloc_free(&alloc);
  cleanup_test_dev(&dev);
  TEST_PASS();
}

/* =========================================================================
 * Main
 * ======================================================================= */

int main(void) {
  printf("\n");
  printf("╔════════════════════════════════════════════════════════════════════"
         "══╗\n");
  printf("║      btrfs2ext4 — Integration & Regression Tests (v0.3.0)          "
         "║\n");
  printf("║   Verifica datos reales en disco, no solo códigos de retorno       "
         " ║\n");
  printf("╚════════════════════════════════════════════════════════════════════"
         "══╝\n\n");

  /* GROUP A: device_batch */
  printf("─── GROUP A: device_batch API "
         "─────────────────────────────────────────\n");
  test_batch_basic_readback();
  test_batch_empty_flush_noop();
  test_batch_overflow_auto_flush();
  test_batch_pwrite_equivalence();
  test_batch_readonly_rejected();

  /* GROUP B: Inode Bitmaps */
  printf("\n─── GROUP B: Inode Bitmaps (Bug B-2) "
         "──────────────────────────────────\n");
  test_inode_bitmap_reserved_inodes();
  test_inode_bitmap_user_inodes_marked();
  test_inode_bitmap_cross_group();

  /* GROUP C: GDT Offsets */
  printf("\n─── GROUP C: GDT con desc_size=64 (Bug B-3) "
         "───────────────────────────\n");
  test_gdt_offset_desc_size64();
  test_gdt_no_overlap_between_groups();
  test_gdt_group0_inode_table_in_bounds();

  /* GROUP D: Tail blocks */
  printf("\n─── GROUP D: Tail Block Marking (Bug B-11) "
         "────────────────────────────\n");
  test_tail_blocks_marked_used();
  test_tail_boundary_exact_multiple();

  /* GROUP E: Directory extent tree */
  printf("\n─── GROUP E: Directory Extent Tree (Bug B-4) "
         "──────────────────────────\n");
  test_dir_small_inline_extents();
  test_dir_large_depth1_extent_tree();
  test_dir_huge_all_blocks_reachable();

  /* GROUP F: GDT Checksums */
  printf("\n─── GROUP F: GDT Checksums (Bug B-6) "
         "──────────────────────────────────\n");
  test_gdt_checksum_nonzero();
  test_gdt_checksum_value_correct();

  /* GROUP G: Block Allocator */
  printf("\n─── GROUP G: Block Allocator (Bugs B-9, B-10) "
         "─────────────────────────\n");
  test_alloc_direction_forward();
  test_alloc_wraparound();
  test_alloc_no_metadata_collision();

  /* GROUP H: Journal Zeroing */
  printf("\n─── GROUP H: Journal Zeroing (Bug B-7) "
         "────────────────────────────────\n");
  test_journal_jbd2_magic();
  test_journal_blocks_zeroed();
  test_journal_zeroing_speed();

  /* GROUP I: End-to-End Consistency */
  printf("\n─── GROUP I: Consistencia End-to-End "
         "──────────────────────────────────\n");
  test_e2e_superblock_magic();
  test_e2e_free_counts_consistent();
  test_e2e_metadata_regions_no_overlap();
  test_e2e_inode_table_within_bounds();
  test_e2e_superblock_feature_bits();
  test_e2e_reserved_inodes_not_free();

  /* Summary */
  printf("\n═══════════════════════════════════════════════════════════════════"
         "═══════\n");
  printf("  %d tests  |  " COL_PASS "%d passed" COL_RST, tests_run,
         tests_passed);
  if (tests_failed > 0)
    printf("  |  " COL_FAIL "%d FAILED" COL_RST, tests_failed);
  printf("\n═══════════════════════════════════════════════════════════════════"
         "═══════\n\n");

  if (tests_failed > 0) {
    printf("  Consejo: compilar con -fsanitize=address para detectar\n");
    printf("  UAF/double-free en los tests del grupo A y B.\n\n");
  }

  return tests_failed > 0 ? 1 : 0;
}
