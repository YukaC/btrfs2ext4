/*
 * superblock.c — Btrfs superblock parser
 *
 * Reads and validates the primary btrfs superblock at offset 0x10000.
 */

#include <endian.h>
#include <stdio.h>
#include <string.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/btrfs_structures.h"
#include "device_io.h"

/* CRC32C (Castagnoli) implementation */
static uint32_t crc32c_table[256];
static int crc32c_initialized = 0;

static void crc32c_init_table(void) {
  uint32_t i, j, crc;
  for (i = 0; i < 256; i++) {
    crc = i;
    for (j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0x82F63B78;
      else
        crc >>= 1;
    }
    crc32c_table[i] = crc;
  }
  crc32c_initialized = 1;
}

uint32_t crc32c(uint32_t crc, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  if (!crc32c_initialized)
    crc32c_init_table();

  crc = ~crc;
  while (len--)
    crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

/*
 * Validate and parse the btrfs superblock.
 * Populates the sb field in fs_info.
 */
int btrfs_read_superblock(struct device *dev, struct btrfs_super_block *sb) {
  /* Read primary superblock at 64 KiB */
  if (device_read(dev, BTRFS_SUPER_OFFSET, sb, sizeof(*sb)) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to read superblock at offset 0x%x\n",
            BTRFS_SUPER_OFFSET);
    return -1;
  }

  /* Validate magic */
  uint64_t magic = le64toh(sb->magic);
  if (magic != BTRFS_MAGIC) {
    fprintf(stderr,
            "btrfs2ext4: invalid btrfs magic: 0x%016lx (expected 0x%016lx)\n",
            (unsigned long)magic, (unsigned long)BTRFS_MAGIC);
    return -1;
  }

  /* Validate checksum (CRC32C of everything after the csum field) */
  uint16_t csum_type = le16toh(sb->csum_type);
  if (csum_type != BTRFS_CSUM_TYPE_CRC32) {
    fprintf(
        stderr,
        "btrfs2ext4: unsupported checksum type %u (only CRC32C supported)\n",
        csum_type);
    return -1;
  }

  uint32_t stored_csum;
  memcpy(&stored_csum, sb->csum, sizeof(uint32_t));
  stored_csum = le32toh(stored_csum);

  uint32_t computed_csum = crc32c(0, (const uint8_t *)sb + BTRFS_CSUM_SIZE,
                                  sizeof(*sb) - BTRFS_CSUM_SIZE);

  if (stored_csum != computed_csum) {
    fprintf(stderr,
            "btrfs2ext4: superblock checksum mismatch: stored=0x%08x "
            "computed=0x%08x\n",
            stored_csum, computed_csum);
    return -1;
  }

  /* Print superblock info */
  printf("=== Btrfs Superblock ===\n");
  printf("  Label:       %s\n", sb->label[0] ? sb->label : "(none)");
  printf("  Generation:  %lu\n", (unsigned long)le64toh(sb->generation));
  printf("  Total bytes: %lu (%.1f GiB)\n",
         (unsigned long)le64toh(sb->total_bytes),
         (double)le64toh(sb->total_bytes) / (1024.0 * 1024.0 * 1024.0));
  printf("  Bytes used:  %lu (%.1f GiB)\n",
         (unsigned long)le64toh(sb->bytes_used),
         (double)le64toh(sb->bytes_used) / (1024.0 * 1024.0 * 1024.0));
  printf("  Sector size: %u\n", le32toh(sb->sectorsize));
  printf("  Node size:   %u\n", le32toh(sb->nodesize));
  printf("  Num devices: %lu\n", (unsigned long)le64toh(sb->num_devices));
  printf("  Root tree:   0x%lx\n", (unsigned long)le64toh(sb->root));
  printf("  Chunk tree:  0x%lx\n", (unsigned long)le64toh(sb->chunk_root));
  printf("  Csum type:   %u (CRC32C)\n", csum_type);
  printf("========================\n\n");

  /* Validate sector size */
  uint32_t sectorsize = le32toh(sb->sectorsize);
  if (sectorsize != 4096) {
    fprintf(
        stderr,
        "btrfs2ext4: unsupported sector size %u (only 4096 supported in v1)\n",
        sectorsize);
    return -1;
  }

  /* Validate nodesize: must be a sane multiple of sectorsize.
   * Btrfs typically uses 4K–64K node sizes; we bound it defensively to
   * avoid gigantic allocations and I/O sizes on corrupt superblocks. */
  uint32_t nodesize = le32toh(sb->nodesize);
  if (nodesize < sectorsize || nodesize > 64 * 1024 ||
      (nodesize % sectorsize) != 0) {
    fprintf(stderr,
            "btrfs2ext4: unsupported or suspicious node size %u "
            "(sector=%u, supported range [%u,%u])\n",
            nodesize, sectorsize, sectorsize, 64 * 1024);
    return -1;
  }

  /* Validate single device */
  uint64_t num_devices = le64toh(sb->num_devices);
  if (num_devices != 1) {
    fprintf(stderr,
            "btrfs2ext4: multi-device btrfs not supported in v1 (found %lu "
            "devices)\n",
            (unsigned long)num_devices);
    return -1;
  }

  return 0;
}
