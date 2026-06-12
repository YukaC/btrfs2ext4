#include <stddef.h>
#include <endian.h>

#include "btrfs/checksum.h"
#include "ext4/ext4_metadata_csum.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"

void ext4_inode_set_checksum_ino(uint32_t csum_seed, struct ext4_inode *inode,
                                 uint32_t inode_size, uint32_t ino) {
  uint32_t le_ino = htole32(ino);
  uint32_t seed = crc32c(csum_seed, &le_ino, sizeof(le_ino));
  seed = crc32c(seed, &inode->i_generation, sizeof(inode->i_generation));

  inode->i_checksum_lo = 0;
  inode->i_checksum_hi = 0;

  uint32_t csum =
      crc32c(seed, inode, offsetof(struct ext4_inode, i_checksum_lo));
  csum = crc32c(csum, &inode->i_dtime,
                EXT4_GOOD_OLD_INODE_SIZE -
                    offsetof(struct ext4_inode, i_dtime));
  if (inode_size > EXT4_GOOD_OLD_INODE_SIZE)
    csum = crc32c(csum, &inode->i_extra_isize,
                  inode_size - EXT4_GOOD_OLD_INODE_SIZE);

  inode->i_checksum_lo = htole16((uint16_t)(csum & 0xFFFF));
  if (inode_size > 128)
    inode->i_checksum_hi = htole16((uint16_t)(csum >> 16));
}

void ext4_inode_set_checksum(const uint8_t uuid[16], struct ext4_inode *inode,
                             uint32_t inode_size) {
  ext4_inode_set_checksum_ino(ext4_csum_seed_from_uuid(uuid), inode, inode_size,
                              0);
}
