# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.1.0-alpha] - 2026-02-24

### Added

- **Hardening v3 Implementation**: Massive security refactor neutralizing potential memory corruption exploits stemming from maliciously crafted Btrfs metadata.
- **Strict Bounds Checking**: Integrated comprehensive `sizeof` checks across generic `btrfs_item` bounds parsing.
- **Integer Overflow Protection**: Deployed `__builtin_add_overflow` to defend block pointer math against extremely high structural allocation manipulation.
- **Stack-Smashing Prevention**: Refactored array depth limits inside recursive Ext4 extent tree logic to bound depth at static, deterministic constraints based exactly on memory architecture properties.
- **Hardware Viability Audit Expansion**: Enhancements to the battery/power level abort mechanisms protecting Pass 3 `ext4` commit sequences.
- **Bloom Filter Performance**: Added `mmap()` Bloom Filter bounded probability tracking to avoid HDD head thrashing.

### Fixed

- Fixed critical Out-Of-Memory (OOM) bugs during Hash Lookup mappings on massive partitions exceeding local system RAM limits.
- Patched an issue where deeply nested Ext4 multi-level HTree layouts misallocated node arrays during high B-Tree traversal depths.
- Addressed theoretical vulnerability allowing overlapping logical volume boundaries on highly corrupted chunk maps.
- Adjusted integer types directly mapping file extents inside 32-bit architectural constraints safely.

### Security

- The built-in testing suite (`test_fuzz.c` and `test_stress.c`) has been drastically expanded to repeatedly fuzz Btrfs malformed limits, ensuring all bounding additions completely prevent ASan warnings.

---

## [0.0.1-pre] - 2026-01-01

### Added **Initial Proof-of-Concept release**: Validates pure zero-copy math conversions from limited `btrfs` B-Tree structures to linear sequential `ext4` layouts. Includes native block coalescing and write-ahead journaling logic.
