#!/usr/bin/env python3
"""Apply Phase 3 Approach C implementation (batch checkpoint sync)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HEADER = r'''/*
 * migration_map.h — Persistent Block Relocation Journal (Phase 3 Approach C)
 */
#ifndef MIGRATION_MAP_H
#define MIGRATION_MAP_H
#include "relocator.h"
#include <stdint.h>
#define MIGRATION_MAGIC "B2E4MAP1"
#define MIGRATION_FOOTER_OFFSET 8192
#define SUPERBLOCK_BACKUP_OFFSET 4096
#define MIGRATION_MAX_ENTRIES (1024U * 1024U)
#define MIGRATION_DEFAULT_CHECKPOINT_INTERVAL 8
#define MIGRATION_CHECKPOINT_BYTES (32ULL * 1024 * 1024)
struct migration_footer {
  char magic[8];
  uint64_t map_offset;
  uint32_t entry_count;
  uint32_t crc32;
  uint32_t checkpoint_interval;
  uint32_t padding[10];
};
struct device;
int migration_map_compute_layout(uint64_t dev_size, uint32_t entry_count,
    uint64_t *map_offset, uint64_t *footer_offset,
    uint64_t *backup_offset, uint64_t *map_size);
int migration_map_footer_present(struct device *dev);
int migration_map_detect_existing(struct device *dev);
uint32_t migration_map_completed_count(const struct relocation_plan *plan);
int migration_map_validate_plan(const struct relocation_entry *entries,
    uint32_t count, uint64_t dev_size);
int migration_map_preflight_space(struct device *dev, uint32_t entry_count);
int migration_map_save(struct device *dev, const struct relocation_plan *plan);
int migration_map_load(struct device *dev, struct relocation_plan *plan);
int migration_map_flush_progress(struct device *dev,
    const struct relocation_plan *plan);
int migration_map_rollback(struct device *dev);
#endif
'''

# migration_map.c loaded from sibling file
MM_C = (ROOT / "scripts" / "migration_map_approach_c.c").read_text()

def patch_relocator(text: str) -> str:
    if '#include "migration_map.h"' not in text:
        text = text.replace(
            '#include "mem_tracker.h"\n#include "relocator.h"',
            '#include "mem_tracker.h"\n#include "migration_map.h"\n#include "relocator.h"',
        )
    if "maybe_flush_checkpoint" in text:
        return text
    old = '''/* ========================================================================
 * Relocation executor — with batched I/O and hash-based extent update
 * ======================================================================== */

int relocator_execute(struct relocation_plan *plan, struct device *dev,
                      struct btrfs_fs_info *fs_info, uint32_t block_size) {
  if (plan->count == 0) {
    printf("No blocks need relocation.\\n\\n");
    return 0;
  }

  printf("Executing %u block relocations...\\n", plan->count);
'''
    new = '''/* Relocation executor — batch checkpoints (Approach C) */
static uint32_t read_checkpoint_interval(struct device *dev) {
  struct migration_footer footer;
  uint64_t bo = (dev->size - SUPERBLOCK_BACKUP_OFFSET) & ~4095ULL;
  uint64_t fo = bo - MIGRATION_FOOTER_OFFSET;
  if (device_read(dev, fo, &footer, sizeof(footer)) < 0)
    return MIGRATION_DEFAULT_CHECKPOINT_INTERVAL;
  if (memcmp(footer.magic, MIGRATION_MAGIC, 8) != 0)
    return MIGRATION_DEFAULT_CHECKPOINT_INTERVAL;
  return footer.checkpoint_interval ? footer.checkpoint_interval
                                    : MIGRATION_DEFAULT_CHECKPOINT_INTERVAL;
}
static int maybe_flush_checkpoint(struct device *dev, struct relocation_plan *plan,
    uint32_t *nent, uint64_t *nbytes, uint32_t interval) {
  if (*nent < interval && *nbytes < MIGRATION_CHECKPOINT_BYTES)
    return 0;
  if (migration_map_flush_progress(dev, plan) < 0)
    return -1;
  *nent = 0;
  *nbytes = 0;
  return 0;
}
int relocator_execute(struct relocation_plan *plan, struct device *dev,
                      struct btrfs_fs_info *fs_info, uint32_t block_size) {
  if (plan->count == 0) {
    printf("No blocks need relocation.\\n\\n");
    return 0;
  }
  uint32_t done0 = migration_map_completed_count(plan);
  printf("Executing %u block relocations (%u already completed)...\\n",
         plan->count, done0);
  uint32_t interval = read_checkpoint_interval(dev);
  uint32_t since = 0;
  uint64_t bytes_since = 0;
'''
    text = text.replace(old, new)
    text = text.replace(
        "  for (uint32_t i = 0; i < plan->count; i++) {\n"
        "    struct relocation_entry *re = &plan->entries[i];\n\n"
        "    uint64_t remaining = re->length;",
        "  for (uint32_t i = 0; i < plan->count; i++) {\n"
        "    struct relocation_entry *re = &plan->entries[i];\n"
        "    if (re->completed)\n"
        "      continue;\n\n"
        "    uint64_t remaining = re->length;",
    )
    text = text.replace(
        "    re->completed = 1;\n\n"
        "    /* Update in-memory extent maps using hash (O(1) per block - supports CoW",
        "    /* Update in-memory extent maps using hash (O(1) per block - supports CoW",
    )
    old_end = '''    /* Progress */
    if ((i + 1) % 100 == 0 || i + 1 == plan->count) {
      printf("  Relocated %u/%u entries (%.1f%%)\\n", i + 1, plan->count,
             100.0 * (i + 1) / plan->count);
    }
  }

  free(buf);
  if (have_hash)
    extent_hash_free(&ehash);
  device_sync(dev);

  printf("  Block relocation complete\\n\\n");
  return 0;
}'''
    new_end = '''    re->completed = 1;
    since++;
    bytes_since += re->length;
    if (maybe_flush_checkpoint(dev, plan, &since, &bytes_since, interval) < 0) {
      free(buf);
      if (have_hash)
        extent_hash_free(&ehash);
      return -1;
    }
    if ((i + 1) % 100 == 0 || i + 1 == plan->count) {
      printf("  Relocated %u/%u entries (%.1f%%)\\n", i + 1, plan->count,
             100.0 * (i + 1) / plan->count);
    }
  }
  if (since > 0 || bytes_since > 0) {
    if (migration_map_flush_progress(dev, plan) < 0) {
      free(buf);
      if (have_hash)
        extent_hash_free(&ehash);
      return -1;
    }
  }
  free(buf);
  if (have_hash)
    extent_hash_free(&ehash);
  printf("  Block relocation complete\\n\\n");
  return 0;
}'''
    return text.replace(old_end, new_end)

def patch_main(text: str) -> str:
    if "migration_map_detect_existing" in text:
        return text
    text = text.replace(
        'printf("Device: %s (%.1f GiB)\\n\\n", opts->device_path,\n'
        '         (double)st.dev.size / (1024.0 * 1024.0 * 1024.0));\n\n'
        '  /* ================================================\n   * PASS 1:',
        'printf("Device: %s (%.1f GiB)\\n\\n", opts->device_path,\n'
        '         (double)st.dev.size / (1024.0 * 1024.0 * 1024.0));\n\n'
        '  if (!opts->dry_run && migration_map_detect_existing(&st.dev) > 0 &&\n'
        '      !opts->force) {\n'
        '    fprintf(stderr,\n'
        '            "btrfs2ext4: existing migration map — use --rollback or --force\\n");\n'
        '    goto cleanup;\n'
        '  }\n\n'
        '  /* ================================================\n   * PASS 1:',
    )
    old = '''  if (!opts->dry_run) {
    if (progress)
      progress("Pass 2", 60, "Saving migration map and btrfs backup...");

    /* plan.v1 fix: Save migration map UNCONDITIONALLY (even when count=0).
     * This ensures a rollback checkpoint exists before Pass 3 writes begin,
     * even if no blocks needed relocation. Without this, a crash during
     * Pass 3 would leave the filesystem in an unrecoverable state. */
    if (migration_map_save(&st.dev, &st.reloc_plan) < 0) {
      fprintf(stderr, "btrfs2ext4: failed to save migration map (aborting to "
                      "prevent data loss)\\n");
      goto cleanup;
    }

    if (st.reloc_plan.count > 0) {
      if (!check_battery_safe()) {
        fprintf(stderr,
                "btrfs2ext4: aborting block relocation — unsafe power state\\n");
        goto cleanup;
      }

      if (progress)
        progress("Pass 2", 70, "Relocating conflicting blocks...");

      if (relocator_execute(&st.reloc_plan, &st.dev, &st.fs_info, st.layout.block_size) <
          0) {
        fprintf(stderr, "btrfs2ext4: block relocation failed!\\n");
        goto cleanup;
      }
    }
  }'''
    new = '''  if (!opts->dry_run) {
    int resuming = migration_map_detect_existing(&st.dev) > 0 && opts->force;
    if (resuming) {
      relocator_free(&st.reloc_plan);
      if (migration_map_load(&st.dev, &st.reloc_plan) < 0) {
        fprintf(stderr, "btrfs2ext4: failed to load migration map\\n");
        goto cleanup;
      }
    } else if (st.reloc_plan.count > 0) {
      if (migration_map_validate_plan(st.reloc_plan.entries, st.reloc_plan.count,
                                      st.dev.size) < 0 ||
          migration_map_preflight_space(&st.dev, st.reloc_plan.count) < 0) {
        fprintf(stderr, "btrfs2ext4: migration plan validation failed\\n");
        goto cleanup;
      }
    }
    if (!resuming) {
      if (progress)
        progress("Pass 2", 60, "Saving migration map and btrfs backup...");
      if (migration_map_save(&st.dev, &st.reloc_plan) < 0) {
        fprintf(stderr, "btrfs2ext4: failed to save migration map\\n");
        goto cleanup;
      }
    }
    if (st.reloc_plan.count > 0) {
      if (!check_battery_safe()) {
        fprintf(stderr,
                "btrfs2ext4: aborting block relocation — unsafe power state\\n");
        goto cleanup;
      }
      if (progress)
        progress("Pass 2", 70, "Relocating conflicting blocks...");
      if (relocator_execute(&st.reloc_plan, &st.dev, &st.fs_info,
                            st.layout.block_size) < 0) {
        fprintf(stderr, "btrfs2ext4: block relocation failed!\\n");
        goto cleanup;
      }
    }
  }'''
    text = text.replace(old, new)
    if '{"force"' not in text:
        text = text.replace(
            '{"memory-limit", required_argument, NULL, \'m\'},',
            '{"memory-limit", required_argument, NULL, \'m\'},\n'
            '      {"force", no_argument, NULL, \'f\'},',
        )
        text = text.replace('"nvb:i:rw:m:hV"', '"nvb:i:rw:m:fhV"')
        text = text.replace(
            "case 'm':\n      opts.memory_limit_mb = (uint32_t)atoi(optarg);\n      break;\n    case 'h':",
            "case 'm':\n      opts.memory_limit_mb = (uint32_t)atoi(optarg);\n      break;\n    case 'f':\n      opts.force = 1;\n      break;\n    case 'h':",
        )
    return text

def main():
    (ROOT / "include" / "migration_map.h").write_text(HEADER)
    (ROOT / "src" / "migration_map.c").write_text(MM_C)
    (ROOT / "src" / "relocator.c").write_text(
        patch_relocator((ROOT / "src" / "relocator.c").read_text())
    )
    (ROOT / "src" / "main.c").write_text(
        patch_main((ROOT / "src" / "main.c").read_text())
    )
    h = (ROOT / "include" / "btrfs2ext4.h").read_text()
    if "int force;" not in h:
        h = h.replace("  int rollback;", "  int rollback;\n  int force;")
        (ROOT / "include" / "btrfs2ext4.h").write_text(h)
    t = (ROOT / "tests" / "test_integration.c").read_text()
    if "extern uint32_t crc32c" not in t:
        t = t.replace(
            '#include "relocator.h"\n',
            '#include "relocator.h"\n\nextern uint32_t crc32c(uint32_t crc, const void *buf, size_t len);\n',
        )
        (ROOT / "tests" / "test_integration.c").write_text(t)

if __name__ == "__main__":
    main()
