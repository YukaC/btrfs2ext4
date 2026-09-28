# SPEC — btrfs2ext4 / in-place Btrfs→Ext4 converter

§G
CLI C convierte Btrfs→Ext4 in-place mismo block device. Hobby/emergencia. ! backup antes. ⊥ pretender prod-battle-tested.

§C
- stack: C11 · CMake · libuuid · zlib · optional OpenSSL/xxhash/lzo/zstd/liburing
- ! destructive: escribe device directo; dry-run `-n` para audit espacio/ETA
- CI: `.github/workflows/ci.yml` matrix gcc/clang × Debug/Release + sanitizers tests
- LICENSE MIT (obra propia)
- docs: `README.md` · `TECHNICAL.md` · `impPlan.md` · `CHANGELOG.md` · `CONTRIBUTING.md`

§I
```
cmd: btrfs2ext4 <device> [-n|--dry-run] [-v|--verbose] [-b N] [-i N] [-w PATH]
file: src/main.c · src/term_ui.c · src/relocator.c · src/ext4/* · src/btrfs/*
file: include/btrfs2ext4.h · include/term_ui.h · include/mem_tracker.h
test: test_checksum · test_fuzz · test_stress · test_integration
ci: reusable-build-test.yml
```

§V
```
V1: dry-run → ⊥ writes destructivos al device
V2: ∀ release build → tests CI verdes antes merge main
V3: mem_tracker bounds → ⊥ OOM unbounded en paths hot
V4: verbose `-v` → dumps diagnósticos; default TTY compact
```

§T
```
id|status|task|cites
T1|x|Phase 0 CI sanitizers exit codes|V2
T2|x|Phase 1–4b space/recovery/ETA|§G
T3|x|Phase 5 pipeline I/O + mem_tracker|V3
T4|x|compact TTY dashboard|V4
T5|.|battle-test more real FS sizes|§G
```

§B
```
id|date|cause|fix
B1|2026-09-28|test targets missing -lm after term_ui fmod|V2
```
