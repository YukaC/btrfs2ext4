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
#include "btrfs/checksum.h"
#include "device_io.h"

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

  /* Validate checksum (CRC32C or others supported) */
  uint16_t csum_type = le16toh(sb->csum_type);
  if (btrfs_csum_size(csum_type) == 0) {
    fprintf(stderr, "btrfs2ext4: unsupported checksum type %u\n", csum_type);
    return -1;
  }

  if (btrfs_verify_checksum(csum_type, sb->csum,
                            (const uint8_t *)sb + BTRFS_CSUM_SIZE,
                            sizeof(*sb) - BTRFS_CSUM_SIZE) != 0) {
    fprintf(stderr,
            "btrfs2ext4: superblock checksum mismatch "
            "(algorithm: %s)\n",
            btrfs_csum_name(csum_type));
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
  printf("  Csum type:   %u (%s)\n", csum_type, btrfs_csum_name(csum_type));
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
