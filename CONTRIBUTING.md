# Contributing to `btrfs2ext4`

> This is a hobby project built in a few days to solve a real problem. There is no company behind it, no engineering team, no formal process. If you found a bug, hit an edge case, or want to add something — you are very welcome. A short issue or email is genuinely useful.

Because this tool writes directly to block devices with root privileges, bugs can destroy data. The bar for correctness is high, but the process is informal.

---

## Engineering principles

These are not ideology — they are why the tool works on constrained hardware:

**1. Bounded memory growth**
Never let arrays or hash tables grow linearly with filesystem size without a cap. On a filesystem with 100M inodes the tool would OOM-kill itself. All large structures must fall back to `mmap()`-backed temp files and use Bloom filters to limit I/O overhead.

**2. Assume worst-case hardware**
15-year-old HDD, 2 GB RAM, 12% battery. This means:

- Always coalesce small writes into sequential runs
- Use `posix_fadvise(POSIX_FADV_WILLNEED)` before B-tree walks
- Never issue millions of random small I/Os

**3. Dry-run must be exact**
`--dry-run` must calculate space and time requirements down to the byte before any write happens. If a feature is added, its space impact must be reflected in the dry-run audit.

---

## Development setup

Language: **C11**, strictly compiled with `-Wall -Wextra`.

**Build:**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**Debug build (AddressSanitizer + UBSan):**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

**Dependencies:** CMake ≥ 3.16, GCC or Clang, `libuuid-devel`, `zlib-devel`. Optional: `lzo-devel`, `libzstd-devel`.

---

## Testing

```bash
cd build && ctest --output-on-failure
```

The test suite (`test_stress.c`) covers:

- Virtual loop device conversions with edge-case filesystems (massive inline files, millions of empty files, overlapping extents)
- Fuzz inputs: malformed superblocks, truncated B-trees, looping symlinks, out-of-bounds references
- ASan / UBSan traps on corrupted input

If your change touches Btrfs parsing, run the fuzz path and confirm no new ASan warnings appear.

---

## Pull request checklist

1. Builds clean with zero warnings in both `Release` and `Debug`
2. `ctest` passes
3. If you change space accounting, update the dry-run math to match
4. Wrap `device_write()` failures to abort cleanly and record to the journal
5. Add a test case if you're fixing a bug or adding a feature

---

## Contact

**agusyuk25@gmail.com** — questions, ideas, larger contributions, or just to say it worked.
