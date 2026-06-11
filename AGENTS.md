# AGENTS.md

`btrfs2ext4` is a single Linux C11 CLI tool (no services, no daemons) that converts a
Btrfs filesystem to Ext4 in place on a block device or image file. Build/test/run commands
are documented in `README.md` and `CONTRIBUTING.md`; prefer those as the source of truth.

## Cursor Cloud specific instructions

Standard build/test commands live in `README.md` ("Build"/"Tests") and `CONTRIBUTING.md`.
Quick reference: configure with `cmake -B build -DCMAKE_BUILD_TYPE=Debug`, build with
`cmake --build build -j$(nproc)`, run tests with `cd build && ctest --output-on-failure`.
The default compiler (`cc`) on this VM is **clang**, not gcc.

Non-obvious caveats discovered during setup:

- **Do NOT install `libssl-dev` (libcrypto).** The optional libcrypto path in
  `src/btrfs/checksum.c` calls `EVP_blake2b256()`, which does not exist in OpenSSL 3.0
  (only `EVP_blake2s256` / `EVP_blake2b512` are exposed), so enabling `HAVE_LIBCRYPTO`
  makes the build fail. `libcrypto` is optional (it only adds SHA256/BLAKE2b *Btrfs*
  checksum support; the default Btrfs checksum is CRC32C via zlib), so the dev environment
  is built without it. The other optional libs (xxhash, lzo2, zstd, liburing) build fine.

- **AddressSanitizer is silently disabled.** In Debug builds `CMakeLists.txt` probes ASan
  via `check_c_compiler_flag(-fsanitize=address ...)`, but that probe omits the linker flag
  and fails to link (`undefined reference to __asan_init`), so `HAS_ASAN` is false and ASan
  is not enabled even though `libasan` works. The test suite passes without it; this is a
  detection quirk, not a missing dependency.

- **The kernel has no btrfs/ext4 modules** (Firecracker VM), so `mount`ing btrfs/ext4
  images fails. You do not need to mount: create a populated test image without mounting
  via `mkfs.btrfs -f --rootdir <staging_dir> image.img`, then run the converter directly on
  the image file. Verify the produced Ext4 with `e2fsck -f -n image.img` / `dumpe2fs`
  (these read the image directly, no mount required).

- **Known alpha bug:** the real conversion (`btrfs2ext4 <image>`, no `-n`) segfaults at the
  "Writing journal" stage on images produced by `mkfs.btrfs` 6.6.3. The `--dry-run` path
  (full Btrfs parse + Ext4 layout planning) and the `integration_test` (synthetic images,
  full write path including journal) both pass, so this is an application robustness bug,
  not an environment problem.
