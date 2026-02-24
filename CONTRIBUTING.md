# Contributing to `btrfs2ext4`

> **This is a hobby project.** It was built by one person to solve a real problem, not backed by a company or dedicated engineering team. If you found it useful, or found something wrong with it — please say so! Opening an issue or dropping an email is genuinely appreciated.

First off, thank you for considering contributing to `btrfs2ext4`! The goal of this project is to provide the safest, most robust, mathematically proven zero-copy filesystem converter ever built.

Because this tool performs destructive, root-level sector operations directly on live block devices, the bar for contributing code is exceptionally high. Predictability, bounded behavior, and data safety take absolute precedence over performance.

---

## 🏗️ 1. Core Engineering Tenets

If you are suggesting a change or feature, please ensure your code adheres to these non-negotiable architectural principles:

### 1. Zero Unbounded Memory Growth (`OOM` Immunity)

Filesystems routinely reach hundreds of millions of inodes and petabytes of data. Under no circumstances should memory arrays, hash tables, or struct maps grow linearly with filesystem size without a strict limit. All extensive tracking **must** be designed around `.tmp` backed `mmap()` structures (SSD swapping) and guarded by Probabilistic Bloom Filters for I/O efficiency. A memory leak or RAM exhaustion will summon the kernel OOM killer, leaving the user with a destroyed partition.

### 2. Zero-Assumption Hardware Capability

We assume the tool is executing on a 15-year old mechanical hard drive with 2GB of RAM and 12% laptop battery remaining.

- You must always fall back effectively from RAM to slow-I/O `mmap()`.
- You cannot issue millions of small sequential write loops (you must coalesce these blocks).
- B-Tree walking must deploy `posix_fadvise(POSIX_FADV_WILLNEED)` to allow underlying kernel prefetching.

### 3. Fail-Safe Dry-Running

If your code adds a feature, the feature **must** be dry-runnable via `--dry-run`. The hardware viability audit must be mathematically exact to the byte across the entire proposed partition expansion before a single write is issued to disk.

---

## 🧑‍💻 2. Development Setup

The `btrfs2ext4` conversion engine is built strictly in C11.

### Dependencies:

- CMake (>= 3.16)
- GCC or Clang (fully warned via `-Wall -Wextra`)
- `libuuid-devel`, `zlib-devel`
- Optional: `lzo-devel`, `libzstd-devel`

### Building the Project:

To build a standard optimized binary:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

For development (enables AddressSanitizer, UndefinedBehaviorSanitizer):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

---

## 🚨 3. Testing and Fuzzing Framework

The testing suite for `btrfs2ext4` goes heavily beyond standard unit tests due to the nature of filesystem conversion. All pull requests **must** pass the integrated fuzzing and stress tests.

### Running the Suite:

```bash
cd build && ctest --output-on-failure
```

### The Fuzzer (`test_fuzz.c`)

The included fuzzer acts as a malicious Btrfs superblock and node corruptor. We feed heavily damaged headers, structurally overlapping extents, intentionally looping symlinks, out-of-bounds references, integer-overflow arrays, and completely mangled `chunk_tree` metadata directly to the reader APIs.
If your code change processes Btrfs input, you **must** verify it does not trigger an ASan trap, stack smash, or infinite loop in `test_fuzz.c`.

### The Viability Audit (`test_stress.c`)

The stress suite builds virtual loop devices loaded with specific combinations of edge cases (10 GB inline files, 5M empty files, heavily overlapping physical extents, legacy directories lacking HTree capacity). Ensure any Ext4 metadata adjustments are registered in the test constraints.

---

## 📌 4. Pull Request Checklist

1. **Review SECURITY.md:** Your code cannot introduce blind buffer assumptions or execute unfiltered commands.
2. **Comment complex math/heuristics:** If you are manipulating logical-to-physical translations or hash layouts, heavily document the exact intent.
3. **Handle partial write failures:** Always wrap `device_write()` failures in `relocator.c` to gracefully abort and register to `journal.c` to let the write-ahead log cleanly undo the damage upon reboot.
4. **Compile cleanly:** `Release` and `Debug` targets must build with zero warnings on current GCC.
5. **Add tests:** Include a new C unit test in `test_stress.c` explicitly validating the new logic or bug you're fixing.

Thank you again for contributing time to keeping filesystems mathematically safe!

---

## 📬 5. Contact & Collaboration

If you have questions, want to discuss a larger contribution, or are just interested in collaborating on something in the IT/systems space, feel free to reach out:

**agusyuk25@gmail.com**

I'm open to collaboration, feedback, and opportunities. No message is too small.
