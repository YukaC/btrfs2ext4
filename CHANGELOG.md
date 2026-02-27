# Changelog

All notable changes to this project will be documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [0.2.0-alpha] - 2026-02-27

### Added

- **Dry-run ETA estimation** — the `-n` mode now runs a 128 MB non-destructive read benchmark on the physical device and uses it alongside exact Ext4 space math to produce a realistic Min/Max Time-to-Completion estimate before any writes occur
- **Thread pool for decompression** — CPU-bound zlib/lzo/zstd decompression now runs in a bounded thread pool; I/O remains single-threaded for HDD compatibility
- **Dynamic directory extent scaling** — `ext4_write_directories` now correctly handles directories that overflow 4 inline allocation blocks, auto-upgrading to a depth-1 HTree B-tree

### Fixed

- **GDT checksum algorithm** — switched `bg_checksum` to the correct little-endian `crc16-ANSI` variant masking the Btrfs UUID, eliminating Group Descriptor validation failures in `e2fsck`
- **Journal persistence race** — fixed a latency issue where zeroed buffer writes from `io_uring` could overlap the JBD2 superblock, corrupting the journal block 0 magic header

---

## [0.1.0-alpha] - 2026-02-24

### Added

- **Security hardening** — comprehensive bounds checking on all `btrfs_item` struct accesses, integer overflow guards via `__builtin_add_overflow`, and explicit depth caps on recursive tree traversal
- **Hardware viability audit expansion** — battery/power-level abort mechanisms now protect the final Ext4 write commit (Phase 3)
- **Bloom filter performance** — `mmap()`-backed Bloom filter for inode lookups, preventing HDD head thrashing on large filesystems

### Fixed

- OOM crash during hash table lookups on filesystems exceeding available RAM
- Ext4 multi-level HTree node misallocation at high B-tree traversal depths
- Overlapping logical volume boundary bug on heavily corrupted chunk maps
- Integer type mismatch in file extent mapping on 32-bit-constrained sizes

---

## [0.0.1-pre] - 2026-01-01

### Added

- Initial proof-of-concept: zero-copy Btrfs → Ext4 math, B-tree walking, sequential block coalescing, and write-ahead journaling
