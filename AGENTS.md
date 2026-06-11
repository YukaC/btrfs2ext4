# btrfs2ext4

In-place Btrfs → Ext4 filesystem converter. Single C11 / CMake CLI product (no services, servers, or databases). See `README.md` and `CONTRIBUTING.md` for usage and the contributor checklist.

## Cursor Cloud specific instructions

Standard build/test/run commands live in `README.md` and `CONTRIBUTING.md`:

- Build (Debug is the CMake default and enables the test targets): `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)`
- Test: `cd build && ctest --output-on-failure`
- Run: `./build/btrfs2ext4 [options] <device-or-image>` (use `-n`/`--dry-run` for a safe read-only audit)

Non-obvious caveats discovered while setting up this environment:

- **Do NOT install `libssl-dev` / enable `libcrypto`.** When `HAVE_LIBCRYPTO` is defined the build fails: `src/btrfs/checksum.c` calls `EVP_blake2b256()`, which does not exist in the system OpenSSL 3.0 (`EVP_blake2s256` does). `libcrypto` is optional and is deliberately excluded from the documented dependency set, so the build stays green. The validated optional libs are `lzo2`, `libzstd`, `libxxhash`, and `liburing`.
- **AddressSanitizer is unavailable in this VM** (`HAS_ASAN` check fails during CMake configure), so Debug builds compile without ASan/UBSan. `ctest` still passes all 4 tests (`stress_test`, `fuzz_test`, `checksum_test`, `integration_test`).
- **The `btrfs` kernel module is not loaded**, so you cannot `mount -o loop` a Btrfs image to populate it with files (mount fails with `unknown filesystem type 'btrfs'`). `mkfs.btrfs` still works for generating test images, and `btrfs2ext4` parses Btrfs images directly without mounting — run it on the unmounted image/device (e.g. `btrfs2ext4 -n -v <image>`).
- **Full in-place conversion (no `-n`) can segfault on a minimal/empty `mkfs.btrfs` image** during the journal-writing phase (this is alpha software with known bugs). For quick manual smoke checks use the dry-run path; rely on `ctest`'s `integration_test` for full-conversion coverage on crafted fixtures.
