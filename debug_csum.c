/*
 * debug_csum.c — Dump btrfs superblock checksum bytes and recompute CRC32c.
 * Usage: sudo ./debug_csum /dev/sdb1
 */
#include <endian.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BTRFS_SUPER_OFFSET 0x10000
#define BTRFS_SUPER_INFO_SIZE 4096
#define BTRFS_CSUM_SIZE 32
#define BTRFS_MAGIC 0x4D5F53665248425FULL

static uint32_t crc32c_table[256];

static void crc32c_init(void) {
  for (int i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78 : (crc >> 1);
    crc32c_table[i] = crc;
  }
}

static uint32_t crc32c(uint32_t crc, const void *data, size_t len) {
  const uint8_t *p = data;
  while (len--)
    crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <device>\n", argv[0]);
    return 1;
  }

  crc32c_init();

  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  /* Read 4096 bytes from offset 0x10000 */
  uint8_t sb[BTRFS_SUPER_INFO_SIZE];
  if (pread(fd, sb, sizeof(sb), BTRFS_SUPER_OFFSET) != sizeof(sb)) {
    perror("pread");
    return 1;
  }
  close(fd);

  /* Magic check */
  uint64_t magic;
  memcpy(&magic, sb + 0x40, 8); /* offset of magic in sb */
  printf("Magic: 0x%016lx (expected 0x%016lx) — %s\n",
         (unsigned long)le64toh(magic), (unsigned long)BTRFS_MAGIC,
         le64toh(magic) == BTRFS_MAGIC ? "OK" : "MISMATCH");

  /* csum_type is at offset of csum(32)+fsid(16)+bytenr(8)+flags(8)+magic(8)+
     generation(8)+root(8)+chunk_root(8)+log_root(8)+log_root_transid(8)+
     total_bytes(8)+bytes_used(8)+root_dir_objectid(8)+num_devices(8)+
     sectorsize(4)+nodesize(4)+unused_leafsize(4)+stripesize(4)+
     sys_chunk_array_size(4)+chunk_root_generation(8)+compat_flags(8)+
     compat_ro_flags(8)+incompat_flags(8) = 32+16+8*12+4*5+8*3 = 32+16+96+20+24
     = 188 */
  /* Easier: use the btrfs-progs known offset: csum_type at byte 0xE4 (228) */
  uint16_t csum_type;
  memcpy(&csum_type, sb + 228, 2);
  csum_type = le16toh(csum_type);
  printf("Checksum type: %u (0=CRC32C, 1=xxHash64, 2=SHA256, 3=BLAKE2b)\n",
         csum_type);

  /* Stored checksum (first BTRFS_CSUM_SIZE bytes of the superblock) */
  printf("Stored csum (first 8 bytes): ");
  for (int i = 0; i < 8; i++)
    printf("%02x ", sb[i]);
  printf("\n");

  if (csum_type == 0) {
    /* Compute CRC32c over bytes [BTRFS_CSUM_SIZE .. 4095] */
    uint32_t crc = crc32c(~0U, sb + BTRFS_CSUM_SIZE,
                          BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
    uint32_t crc_le = htole32(crc);
    printf("Computed CRC32c (seed ~0U, NO final invert): 0x%08x\n", crc);
    printf("Computed bytes: %02x %02x %02x %02x\n", ((uint8_t *)&crc_le)[0],
           ((uint8_t *)&crc_le)[1], ((uint8_t *)&crc_le)[2],
           ((uint8_t *)&crc_le)[3]);

    /* Also try with final invert */
    uint32_t crc_inv = ~crc;
    uint32_t crc_inv_le = htole32(crc_inv);
    printf("Computed CRC32c (WITH final invert):          0x%08x\n", crc_inv);
    printf("Computed bytes: %02x %02x %02x %02x\n", ((uint8_t *)&crc_inv_le)[0],
           ((uint8_t *)&crc_inv_le)[1], ((uint8_t *)&crc_inv_le)[2],
           ((uint8_t *)&crc_inv_le)[3]);

    /* Also try seed 0 (no initial invert) */
    uint32_t crc0 = crc32c(0, sb + BTRFS_CSUM_SIZE,
                           BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
    printf("Computed CRC32c (seed 0, no invert):          0x%08x\n", crc0);

    int match_no_inv = (memcmp(sb, &crc_le, 4) == 0);
    int match_inv = (memcmp(sb, &crc_inv_le, 4) == 0);
    printf("Match (no invert): %s\n", match_no_inv ? "YES" : "NO");
    printf("Match (inverted):  %s\n", match_inv ? "YES" : "NO");
  }

  return 0;
}
