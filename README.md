# btrfs2ext4

**In-place Btrfs → Ext4 filesystem converter**

> [!CAUTION]
> **Destructive, irreversible operation.** Always back up your data before converting. This tool writes directly to your block device.

---

## The story

I had a pc with a tiny ssd and a old mechanical hard drive of 1tb with a large Btrfs partition of 800gb. I needed to convert to Ext4. No spare disk. Not enough free space anywhere for a full copy. Every existing tool either required an intermediate drive, consumed absurd amounts of RAM (OOM-killing itself before finishing), or thrashed the HDD so hard the conversion would have taken days — if it ever finished.

So I built `btrfs2ext4` over a weekend to solve exactly that problem.

It reads Btrfs metadata, plans an Ext4 layout on the same device, relocates any blocks that would collide, and writes the new structures in a single pass — no intermediate copy, no extra drive. It is aggressively hardware-aware: it coalesces I/O to avoid seek storms on HDDs, bounds memory usage to prevent OOM crashes, and checks battery level before writing anything critical.

**This is a hobby project.** It was written fast, for a real emergency, by one person. There are almost certainly bugs, edge cases, and missing features. It has been tested on a handful of real filesystems and a stress/fuzz suite, but it has not been battle-tested at scale. Use it with full awareness of that.

---

## What it does

`btrfs2ext4` converts a Btrfs filesystem to Ext4 **directly on the same block device or image file**. It:

- Parses the Btrfs B-tree metadata (chunk tree, FS tree, inodes, extents, directories, xattrs)
- Plans a full Ext4 layout (superblock, GDT, bitmaps, inode tables, journal)
- Detects and relocates any Btrfs data blocks that overlap Ext4 metadata positions
- Decompresses zlib / lzo / zstd extents on the fly
- Resolves Btrfs CoW shared extents (clone/snapshot deduplication)
- Writes Ext4 structures with a write-ahead journal for crash recovery
- Preserves permissions, ownership, timestamps (ns precision), symlinks, hardlinks, device nodes, xattrs, and ACLs

---

## Best and worst case scenarios

**Best case:** An unmounted, single-device Btrfs partition on an SSD, no compression, no snapshots, plenty of free space, full battery, ample RAM. The conversion runs fast and `e2fsck -f` passes clean on the first try.

**Worst case:** A heavily fragmented, heavily compressed Btrfs volume with snapshot history, on an old HDD with 2 GB of RAM and a half-charged laptop battery. In this case:

- The decompressed data may be significantly larger than the compressed on-disk size — the dry-run will catch this and abort if there is not enough space
- Relocation of conflicting blocks will be slow due to HDD seek times
- The mmap() swap fallback will activate, adding I/O overhead
- Conversion time may be measured in hours

The tool is designed to handle the worst case safely. It will tell you upfront (via `--dry-run`) whether the conversion is viable, give you a time estimate, and refuse to proceed if conditions are unsafe (insufficient space, failing hardware, critical battery).

---

## Known limitations

- **Single-device Btrfs only** — no RAID / multi-device volumes
- **4 KiB sector size only** — non-4K sector sizes are not supported
- **Alpha software** — bugs are expected; always run `--dry-run` first and verify with `e2fsck -f` after
- No support for Btrfs subvolume selection — converts the default subvolume tree

---

## Platform

**Linux only.** Requires Linux-specific syscalls (`BLKGETSIZE64`, `posix_fadvise`, `fdatasync`, `/proc/meminfo`). Tested on x86-64 and ARM64.

---

## Dependencies

| Dependency        | Purpose                             | Required? |
| ----------------- | ----------------------------------- | --------- |
| CMake ≥ 3.16      | Build system                        | ✅        |
| GCC / Clang (C11) | Compiler                            | ✅        |
| `libuuid`         | UUID generation for Ext4 superblock | ✅        |
| `zlib`            | CRC32 / zlib decompression          | ✅        |
| `lzo2`            | LZO decompression (Btrfs data)      | Optional  |
| `libzstd`         | Zstd decompression (Btrfs data)     | Optional  |
| `libcrypto`       | SHA256 / BLAKE2b checksum support   | Optional  |
| `libxxhash`       | xxHash64 checksum support           | Optional  |
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

The binary is at `build/btrfs2ext4`.

### Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

---

## Usage

```
btrfs2ext4 [options] <device>
```

| Option                       | Description                                        |
| ---------------------------- | -------------------------------------------------- |
| `-n`, `--dry-run`            | Simulate the conversion (space + time audit)       |
| `-v`, `--verbose`            | Enable verbose output                              |
| `-b N`, `--block-size N`     | Ext4 block size: 1024, 2048, or **4096** (default) |
| `-i N`, `--inode-ratio N`    | Bytes-per-inode ratio (default: 16384)             |
| `-w PATH`, `--workdir PATH`  | Directory for mmap() swap files (default: ./)      |
| `-m LIMIT`, `--memory-limit` | Memory threshold for mmap, in bytes or `%`         |
| `-r`, `--rollback`           | Restore original Btrfs superblock from backup      |
| `-V`, `--version`            | Print version                                      |
| `-h`, `--help`               | Print help                                         |

### Typical workflow

```bash
# 1. Unmount
sudo umount /dev/sdX1

# 2. Dry-run first — checks space, estimates time, detects problems
sudo btrfs2ext4 -n /dev/sdX1

# 3. Convert
sudo btrfs2ext4 /dev/sdX1

# 4. Verify the result
sudo e2fsck -f /dev/sdX1

# 5. Mount
sudo mount /dev/sdX1 /mnt
```

### Rollback

If something goes wrong:

```bash
sudo btrfs2ext4 -r /dev/sdX1
sudo btrfs check /dev/sdX1
```

> [!NOTE]
> Rollback restores the Btrfs superblock from the automatic backup written at the start of conversion. Relocated blocks stay at their new positions — run `btrfs check` to verify integrity.

---

## Possible improvements

Things the tool doesn't do yet but reasonably could:

- Multi-device / RAID Btrfs support
- Subvolume selection
- Non-4K sector size support
- Better progress reporting
- `io_uring` for async I/O on modern kernels

Contributions and bug reports are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Licence

[MIT](LICENSE.md) — free to use, modify, and distribute.
