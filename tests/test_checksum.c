
#include "btrfs/btrfs_structures.h"
#include "btrfs/checksum.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int test_rfc3720_crc32c() {
  const char *data = "123456789";
  uint32_t expected = 0xE3069283;
  uint32_t computed = crc32c(0, data, 9);
  if (computed != expected) {
    printf("FAIL: RFC3720 CRC32C expected 0x%08X, got 0x%08X\n", expected,
           computed);
    return -1;
  }
  printf("PASS: RFC3720 CRC32C\n");
  return 0;
}

int test_btrfs_crc32c_verify() {
  const char *data = "123456789";
  // Standard CRC32c of "123456789" is 0xE3069283.
  // Btrfs uses seed ~0U with a final bitwise invert — which is exactly the
  // same as standard RFC 3720 CRC32c. So the stored checksum on disk IS
  // 0xE3069283 (confirmed against a real Btrfs volume).
  uint32_t expected_le = 0xE3069283;

  uint8_t stored_csum[4];
  stored_csum[0] = (expected_le >> 0) & 0xFF;
  stored_csum[1] = (expected_le >> 8) & 0xFF;
  stored_csum[2] = (expected_le >> 16) & 0xFF;
  stored_csum[3] = (expected_le >> 24) & 0xFF;

  if (btrfs_verify_checksum(BTRFS_CSUM_TYPE_CRC32, stored_csum, data, 9) != 0) {
    printf("FAIL: Btrfs CRC32c verify\n");
    return -1;
  }
  printf("PASS: Btrfs CRC32c verify\n");
  return 0;
}

int main() {
  int errors = 0;

  printf("=== btrfs2ext4 Checksum tests ===\n");
  if (test_rfc3720_crc32c() != 0)
    errors++;
  if (test_btrfs_crc32c_verify() != 0)
    errors++;

  if (errors == 0) {
    printf("All checksum tests passed!\n");
    return 0;
  } else {
    printf("%d tests failed.\n", errors);
    return 1;
  }
}
