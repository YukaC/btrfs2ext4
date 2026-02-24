# btrfs2ext4

**In-place Btrfs → Ext4 filesystem converter**

`btrfs2ext4` converts a Btrfs filesystem to Ext4 directly on the same block device or image file — no intermediate copy required. It reads all Btrfs metadata, plans a compatible Ext4 layout, safely relocates any data blocks that collide with Ext4 metadata positions, and writes the new Ext4 structures in a single pass.

> [!CAUTION]
> This tool performs **destructive, in-place** filesystem conversion. Always back up your data before running it.

---

## Origin & Purpose

**How it started**  
This project was born out of a very specific, frustrating necessity: I needed to convert a massive, heavily fragmented Btrfs partition to Ext4 directly on a laptop with an older mechanical hard drive. I didn't have a spare drive with enough capacity to hold a full backup or perform a standard data copy.

Existing tools were simply inadequate for this reality. They either required an intermediate drive, lacked robustness, consumed excessive memory leading to fatal Out-Of-Memory (OOM) crashes on filesystems with millions of inodes, or caused severe disk thrashing that made the conversion practically impossible to finish.

**Why I made it**  
I built `btrfs2ext4` to solve these exact problems. I needed an **in-place** converter that was heavily hardware-aware, strictly bounded in memory, and engineered for worst-case scenarios.

Every feature in this tool—from the `mmap()` swap pagers and Bloom Filter fallbacks for slow HDDs, to the laptop battery viability audits and coalesced relocations—was designed to guarantee that the conversion succeeds safely and efficiently, even on severely limited hardware.

---

## Features

- **In-place conversion** — no spare disk needed; works on live block devices and image files
- **Dry-run mode with integrity checks** — simulate the entire conversion read-only and physically read conflicting blocks to detect bad sectors before committing
- **Crash-recovery journal** — write-ahead logging for block relocations so an interrupted conversion can be rolled back
- **Native Ext4 journal** — generates a fully structured JBD2 journal (inode 8) during conversion
- **Rollback support** — restore the original Btrfs superblock from an automatic backup
- **Full metadata fidelity** — preserves file/directory permissions, ownership, timestamps (including nanosecond precision and creation time), symlinks, device nodes, and exact multi-parent hard-links
- **Multi-level extent trees** — arbitrary-depth Ext4 extent tree supports files with hundreds of thousands of extents (ideal for heavily fragmented Btrfs volumes)
- **Massive Directory HTrees** — dynamically computes and allocates Ext4 2-level HTree directory indexes for directories spanning multiple blocks or gigabytes
- **Transparent decompression** — automatically decompresses zlib, lzo, and zstd compressed Btrfs extents, with dynamic extent splitting to fit fragmented physical Ext4 free space
- **Memory-aware scaling** — dynamically tracks memory and seamlessly falls back to `mmap()` disk-backed swap files for huge structs over 16 MiB preventing OOM deaths on massive filesystems
- **Strict Hardware Viability Audits** — precisely calculates physical footprint requirements down to the byte (including decompression inflation and CoW duplication), aborting safely if space is insufficient
- **Hardware-agnostic I/O linearization** — sorts Ext4 assignments sequentially to heavily minimize seek times on mechanical HDDs
- **Graceful degradation** — protects slower drives using a pre-cached Bloom filter to prevent disk thrashing during hash table lookups
- **Physical CoW deduplication resolution** — physically clones shared extents to prevent Ext4 "Multiply-Claimed Blocks" corruption when dealing with Btrfs snapshots
- **Operational Safety** — automatically audits laptop battery levels to prevent destructive metadata corruption caused by power loss during Phase 3
- **Native inline files** — packs tiny files (<60-150 bytes) directly into the Ext4 inode (`EXT4_INLINE_DATA_FL`) avoiding block allocations
- **Extended Attributes & ACLs** — rescues Btrfs Xattrs (Security, System, User) into Ext4 trailing residual space
- **Sparse super** — only writes superblock backups at groups 0, 1, and powers of 3/5/7 to minimise overhead
- **64-bit & flex_bg** — generates a modern Ext4 layout with `64bit`, `extents`, `flex_bg`, `filetype`, `dir_index` and `has_journal` features enabled

### Performance optimisations

| Optimisation                           | Before                                  | After                              |
| -------------------------------------- | --------------------------------------- | ---------------------------------- |
| Conflict detection bitmap              | O(N×M) linear scan                      | O(1) per block                     |
| Hash-based inode map                   | O(N²) total lookups                     | O(N) total (amortised O(1) each)   |
| Coalesced relocations                  | One I/O per block                       | One I/O per contiguous run         |
| Extent-map hash for relocation updates | O(inodes × extents) per move            | O(1) per move                      |
| Bitmap block allocator                 | O(N) reserved-block scan per allocation | O(1) bitmap test                   |
| B-tree readahead hints                 | Synchronous node reads                  | `posix_fadvise(WILLNEED)` prefetch |
| Mmap Swap Pagers                       | Fatal Memory Exhaustion (OOM)           | Dynamically bounded to % of RAM    |
| Bloom Filter Fallback                  | HDD I/O Thrashing on lookup miss        | Instant probabilistic filter       |
| I/O Target Linearizer                  | Fragmented `inode` layout distribution  | Sequential assignment via `qsort`  |

---

## Platform

> [!IMPORTANT]
> **Linux only.** `btrfs2ext4` relies on Linux-specific APIs — `BLKGETSIZE64` ioctl, `posix_fadvise()`, `fdatasync()`, and `/proc/meminfo` — and is not portable to macOS, Windows, or BSD. It has been tested on x86-64 and ARM64.

---

## Requirements

| Dependency        | Purpose                             | Required? |
| ----------------- | ----------------------------------- | --------- |
| CMake ≥ 3.16      | Build system                        | ✅        |
| GCC / Clang (C11) | Compiler                            | ✅        |
| `libuuid`         | UUID generation for Ext4 superblock | ✅        |
| `zlib`            | CRC32 / compression support         | ✅        |
| `libcrypto`       | SHA256 / BLAKE2b checksum support   | Optional  |
| `libxxhash`       | xxHash64 checksum support           | Optional  |
| `lzo2`            | LZO decompression (Btrfs data)      | Optional  |
| `libzstd`         | Zstd decompression (Btrfs data)     | Optional  |
| `pkg-config`      | Library discovery                   | ✅        |

**Fedora / RHEL:**

```bash
sudo dnf install cmake gcc libuuid-devel zlib-devel lzo-devel libzstd-devel
```

**Debian / Ubuntu:**

```bash
sudo apt install cmake gcc uuid-dev zlib1g-dev liblzo2-dev libzstd-dev
```

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary is produced at `build/btrfs2ext4`.

### Run the test suite

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # enables ASan if available
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

---

## Usage

```
btrfs2ext4 [options] <device>
```

| Option                        | Description                                         |
| ----------------------------- | --------------------------------------------------- |
| `-n`, `--dry-run`             | Simulate the conversion (Hardware Viability Audit)  |
| `-v`, `--verbose`             | Enable verbose output                               |
| `-b N`, `--block-size N`      | Ext4 block size: 1024, 2048, or **4096** (default)  |
| `-i N`, `--inode-ratio N`     | Bytes-per-inode ratio (default: 16384)              |
| `-w PATH`, `--workdir PATH`   | Directory for large mmap() swap files (default: ./) |
| `-m LIMIT`, `--memory-limit ` | Memory threshold for mmap in bytes or `%` (ex: 75%) |
| `-r`, `--rollback`            | Roll back a previous conversion                     |
| `-V`, `--version`             | Print version                                       |
| `-h`, `--help`                | Print help                                          |

### Typical workflow

```bash
# 1. Unmount the filesystem
sudo umount /dev/sdX1

# 2. Dry-run first to check for issues
sudo btrfs2ext4 -n /dev/sdX1

# 3. Convert
sudo btrfs2ext4 /dev/sdX1

# 4. Post-conversion: verify metadata
sudo e2fsck -f /dev/sdX1

# 5. Mount
sudo mount /dev/sdX1 /mnt
```

### Rollback

If something goes wrong (or you change your mind):

```bash
sudo btrfs2ext4 -r /dev/sdX1
sudo btrfs check /dev/sdX1
```

> [!NOTE]
> Rollback restores the Btrfs superblock from the automatic backup written during conversion. If blocks were relocated, data remains at the new locations — run `btrfs check` to verify integrity.

---

## Limitations (v0.1.0-alpha)

- **Single-device only** — multi-device / RAID Btrfs is not supported
- **4 KiB sector size only** — the only sector size supported in v1

---

## Project layout

```
btrfs2ext4/
├── CMakeLists.txt
├── include/
│   ├── btrfs/
│   │   ├── btrfs_structures.h   # On-disk Btrfs format (packed structs)
│   │   ├── btrfs_reader.h       # In-memory FS representation & reader API
│   │   └── chunk_tree.h         # Logical→physical address resolver
│   ├── ext4/
│   │   ├── ext4_structures.h    # On-disk Ext4 format (packed structs)
│   │   ├── ext4_planner.h       # Layout planning structs & API
│   │   └── ext4_writer.h        # Writer & inode-map API
│   ├── btrfs2ext4.h             # Top-level conversion API
│   ├── device_io.h              # Byte-level I/O abstraction
│   ├── journal.h                # Crash-recovery journal
│   └── relocator.h              # Block-relocation engine
├── src/
│   ├── btrfs/
│   │   ├── superblock.c         # Superblock parser + Integrator
│   │   ├── checksum.c           # CRC32C, xxHash64, SHA256, BLAKE2b validation
│   │   ├── chunk_tree.c         # Chunk tree walker & resolver
│   │   ├── btree.c              # Generic B-tree DFS walker
│   │   └── fs_tree.c            # FS tree reader (inodes, dirs, extents)
│   ├── ext4/
│   │   ├── planner.c            # Block-group layout calculator
│   │   ├── superblock_writer.c  # Ext4 superblock + backup writer
│   │   ├── gdt_writer.c         # Group Descriptor Table writer
│   │   ├── bitmap_writer.c      # Block & inode bitmap writer
│   │   ├── inode_writer.c       # Inode table translator & writer
│   │   ├── dir_writer.c         # Directory entry builder
│   │   └── extent_writer.c      # Extent tree builder + block allocator
│   ├── relocator.c              # Conflict detection & block mover
│   ├── journal.c                # Write-ahead journal implementation
│   ├── device_io.c              # pread/pwrite I/O layer
│   └── main.c                   # CLI entry point & orchestration
└── tests/
    └── test_stress.c            # 33-test stress / vuln / perf suite
```

---

## Licence

[MIT](LICENSE) — free to use, modify, and distribute. See the `LICENSE` file for full details.

---

## Contributing

This is a **personal hobby project** born out of a very real hardware emergency. I'm not a company — just someone who needed a tool that didn't exist and built it.

That said, contributions are very welcome! If you run into a bug, hit an edge case, or have an idea:

- **Open an issue** — even a short one is helpful. The more detail the better (filesystem size, hardware, error output).
- **Submit a pull request** — please run the full test suite first and verify with `e2fsck -fn` on a converted image.
- **Reach out directly** — if you'd like to collaborate or have something bigger in mind, feel free to contact me at **agusyuk25@gmail.com**. I'm also open to opportunities in the IT world.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full contributor guide, including the core engineering tenets and testing requirements.
