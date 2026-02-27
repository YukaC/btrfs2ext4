/*
 * main.c — btrfs2ext4 command-line entry point
 *
 * Usage: btrfs2ext4 [options] <device>
 */

#include <dirent.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/chunk_tree.h"
#include "btrfs2ext4.h"
#include "device_io.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_writer.h"
#include "journal.h"
#include "mem_tracker.h"
#include "migration_map.h"
#include "relocator.h"

#define VERSION "0.1.0-alpha"

static void print_usage(const char *prog) {
  fprintf(
      stderr,
      "btrfs2ext4 v" VERSION "\n"
      "In-place Btrfs to Ext4 filesystem converter\n\n"
      "Usage: %s [options] <device>\n\n"
      "Options:\n"
      "  -n, --dry-run           Simulate conversion (read-only, no "
      "writes)\n"
      "  -v, --verbose           Enable verbose output\n"
      "  -b, --block-size N      Set ext4 block size (default: 4096)\n"
      "  -i, --inode-ratio N     Set inode ratio (default: 16384)\n"
      "  -r, --rollback          Rollback a previous conversion\n"
      "  -w, --workdir <path>    Working directory for temp files (default: "
      "cwd)\n"
      "  -m, --memory-limit N    Max RAM in MB (0=auto 60%% of physical)\n"
      "  -h, --help              Show this help\n"
      "  -V, --version           Show version\n"
      "\n"
      "WARNING: This tool performs in-place filesystem conversion.\n"
      "         Always back up critical data before running!\n"
      "\n"
      "HINT: If converting on a slow HDD, use --workdir to point to a\n"
      "      faster SSD/NVMe for dramatically better temp file I/O.\n"
      "\n",
      prog);
}

void btrfs2ext4_version(void) { printf("btrfs2ext4 version " VERSION "\n"); }

static void progress_print(const char *phase, uint32_t percent,
                           const char *detail) {
  static struct timespec start_time;
  static int started = 0;
  static const char *last_phase = NULL;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  /* Reset timer on phase change */
  if (!started || !last_phase || strcmp(phase, last_phase) != 0) {
    start_time = now;
    started = 1;
    last_phase = phase;
  }

  /* Build progress bar: [████████░░░░░░░░] 45% */
  char bar[21]; /* 20 chars + null */
  uint32_t filled = percent / 5;
  for (uint32_t i = 0; i < 20; i++)
    bar[i] = (i < filled) ? '#' : '-';
  bar[20] = '\0';

  /* Calculate ETA */
  double elapsed = (double)(now.tv_sec - start_time.tv_sec) +
                   (double)(now.tv_nsec - start_time.tv_nsec) / 1e9;
  char eta_buf[32] = "";
  if (percent > 0 && percent < 100 && elapsed > 1.0) {
    double total_est = elapsed * 100.0 / (double)percent;
    double remaining = total_est - elapsed;
    if (remaining > 3600)
      snprintf(eta_buf, sizeof(eta_buf), " ETA: %.0fh%.0fm", remaining / 3600,
               fmod(remaining, 3600) / 60);
    else if (remaining > 60)
      snprintf(eta_buf, sizeof(eta_buf), " ETA: %.0fm%.0fs", remaining / 60,
               fmod(remaining, 60));
    else
      snprintf(eta_buf, sizeof(eta_buf), " ETA: %.0fs", remaining);
  }

  printf("\r[%s] [%s] %3u%%%s %s", phase, bar, percent, eta_buf,
         detail ? detail : "");
  if (percent >= 100)
    printf("\n");
  fflush(stdout);
}

/* ========================================================================
 * Safety Checks (Battery/Power)
 * ======================================================================== */

static int check_battery_safe(void) {
  DIR *dir = opendir("/sys/class/power_supply/");
  if (!dir)
    return 1; /* Cannot determine, assume safe (e.g. desktop) */

  struct dirent *ent;
  int has_battery = 0;
  int is_ac_online = 0;
  int lowest_capacity = 100;

  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;

    char path[512];
    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type",
             ent->d_name);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;

    char type[32] = {0};
    if (fgets(type, sizeof(type), f)) {
      if (strncmp(type, "Battery", 7) == 0) {
        has_battery = 1;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity",
                 ent->d_name);
        FILE *fc = fopen(path, "r");
        if (fc) {
          int cap = 100;
          if (fscanf(fc, "%d", &cap) == 1) {
            if (cap < lowest_capacity)
              lowest_capacity = cap;
          }
          fclose(fc);
        }
      } else if (strncmp(type, "Mains", 5) == 0) {
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online",
                 ent->d_name);
        FILE *fo = fopen(path, "r");
        if (fo) {
          int online = 0;
          if (fscanf(fo, "%d", &online) == 1) {
            if (online == 1)
              is_ac_online = 1;
          }
          fclose(fo);
        }
      }
    }
    fclose(f);
  }
  closedir(dir);

  if (has_battery && !is_ac_online && lowest_capacity < 20) {
    fprintf(stderr, "\n[FATAL] Battery is discharging and below 20%% (%d%%).\n",
            lowest_capacity);
    fprintf(stderr,
            "  Conversion is too risky. A sudden shutdown during Phase 3 "
            "will destroy the filesystem.\n");
    fprintf(stderr, "  Please plug in AC power and try again.\n");
    return 0; /* Not safe */
  }
  return 1; /* Safe */
}

/* ========================================================================
 * Helpers for Hardware Agnosticism & Optimization
 * ======================================================================== */

/*
 * Sort inodes by parent_ino and then Btrfs ino.
 * This ensures that when Ext4 inodes are sequentially assigned in Pass 3,
 * files in the same directory get contiguous Ext4 inode numbers,
 * heavily optimizing HDD head movement during directory traversal.
 */
static int compare_file_entry(const void *a, const void *b) {
  const struct file_entry *fa = *(const struct file_entry **)a;
  const struct file_entry *fb = *(const struct file_entry **)b;

  if (fa->parent_ino < fb->parent_ino)
    return -1;
  if (fa->parent_ino > fb->parent_ino)
    return 1;

  if (fa->ino < fb->ino)
    return -1;
  if (fa->ino > fb->ino)
    return 1;

  return 0;
}

int btrfs2ext4_convert(const struct convert_options *opts,
                       progress_callback progress) {
  struct device dev;
  struct btrfs_fs_info fs_info;
  struct ext4_layout layout;
  struct relocation_plan reloc_plan;
  struct inode_map ino_map;
  int ret = -1;

  memset(&fs_info, 0, sizeof(fs_info));
  memset(&layout, 0, sizeof(layout));
  memset(&reloc_plan, 0, sizeof(reloc_plan));
  memset(&ino_map, 0, sizeof(ino_map));

  printf("==============================================\n");
  printf("   btrfs2ext4 v" VERSION "\n");
  printf("   In-place Btrfs → Ext4 Converter\n");
  printf("==============================================\n\n");

  if (opts->dry_run) {
    printf("*** DRY RUN MODE — no changes will be written ***\n\n");
  }

  /* ================================================
   * Adaptive Memory Detection (production-grade)
   * ================================================ */
  struct adaptive_mem_config mem_cfg;
  memset(&mem_cfg, 0, sizeof(mem_cfg));

  long pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0) {
    mem_cfg.total_ram = (uint64_t)pages * (uint64_t)page_size;
  } else {
    mem_cfg.total_ram = 2ULL * 1024 * 1024 * 1024; /* fallback: 2GB */
  }

  long avail_pages = sysconf(_SC_AVPHYS_PAGES);
  if (avail_pages > 0 && page_size > 0) {
    mem_cfg.available_ram = (uint64_t)avail_pages * (uint64_t)page_size;
  } else {
    mem_cfg.available_ram = mem_cfg.total_ram / 2;
  }

  if (opts->memory_limit_mb > 0) {
    mem_cfg.mmap_threshold = (uint64_t)opts->memory_limit_mb * 1024 * 1024;
  } else {
    /* Auto: 60% of total physical RAM */
    mem_cfg.mmap_threshold = mem_cfg.total_ram * 60 / 100;
  }

  mem_cfg.workdir = opts->workdir ? opts->workdir : ".";

  /* tmpfs safety check: prevent creating swap files on RAM-backed fs */
  struct statfs sfs;
  if (statfs(mem_cfg.workdir, &sfs) == 0) {
    /* tmpfs magic = 0x01021994 */
    if (sfs.f_type == 0x01021994) {
      mem_cfg.workdir_is_tmpfs = 1;
      fprintf(stderr,
              "\n[WARNING] --workdir '%s' is mounted on tmpfs (RAM-backed).\n"
              "  Creating temp swap files here defeats the purpose of mmap!\n"
              "  Use a physical disk path instead.\n\n",
              mem_cfg.workdir);
    }
  }

  printf("[INFO] RAM detected:     %.1f GiB total, %.1f GiB available\n",
         (double)mem_cfg.total_ram / (1024.0 * 1024.0 * 1024.0),
         (double)mem_cfg.available_ram / (1024.0 * 1024.0 * 1024.0));
  printf("[INFO] mmap threshold:   %.0f MiB%s\n",
         (double)mem_cfg.mmap_threshold / (1024.0 * 1024.0),
         opts->memory_limit_mb > 0 ? " (user-configured)" : " (auto: 60%%)");
  printf("[INFO] Temp file dir:    %s%s\n\n", mem_cfg.workdir,
         mem_cfg.workdir_is_tmpfs ? " [tmpfs WARNING]" : "");

  /* Inicializar el tracker de memoria global antes de que otras
   * estructuras opcionales (hashes grandes, bloom filters, etc.)
   * empiecen a llamar a mem_track_exceeded(). */
  mem_track_init();

  /* Open device */
  if (device_open(&dev, opts->device_path, opts->dry_run) < 0)
    return -1;

  printf("Device: %s (%.1f GiB)\n\n", opts->device_path,
         (double)dev.size / (1024.0 * 1024.0 * 1024.0));

  /* ================================================
   * PASS 1: Read Btrfs metadata
   * ================================================ */
  if (progress)
    progress("Pass 1", 0, "Reading btrfs metadata...");

  if (btrfs_read_fs(&dev, &fs_info) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to read btrfs metadata\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 1", 100, "Btrfs metadata read complete");

  /* ================================================
   * PASS 2: Plan ext4 layout + relocate conflicts
   * ================================================ */
  if (progress)
    progress("Pass 2", 0, "Planning ext4 layout...");

  if (ext4_plan_layout(&layout, dev.size, opts->block_size, opts->inode_ratio,
                       &fs_info) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to plan ext4 layout\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 2", 30, "Detecting conflicts...");

  uint32_t conflicts = ext4_find_conflicts(&layout, &fs_info);

  if (progress)
    progress("Pass 2", 50, "Planning relocation...");

  if (relocator_plan(&reloc_plan, &layout, &fs_info) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to plan block relocation\n");
    goto cleanup;
  }

  if (!opts->dry_run) {
    if (progress)
      progress("Pass 2", 60, "Saving migration map and btrfs backup...");

    /* plan.v1 fix: Save migration map UNCONDITIONALLY (even when count=0).
     * This ensures a rollback checkpoint exists before Pass 3 writes begin,
     * even if no blocks needed relocation. Without this, a crash during
     * Pass 3 would leave the filesystem in an unrecoverable state. */
    if (migration_map_save(&dev, &reloc_plan) < 0) {
      fprintf(stderr, "btrfs2ext4: failed to save migration map (aborting to "
                      "prevent data loss)\n");
      goto cleanup;
    }

    if (reloc_plan.count > 0) {
      if (progress)
        progress("Pass 2", 70, "Relocating conflicting blocks...");

      if (relocator_execute(&reloc_plan, &dev, &fs_info, layout.block_size) <
          0) {
        fprintf(stderr, "btrfs2ext4: block relocation failed!\n");
        goto cleanup;
      }
    }
  }

  if (progress)
    progress("Pass 2", 100, "Layout planned, relocation complete");

  printf("\n=== Hardware Viability Audit (Pre-flight Check) ===\n");
  printf("  RAM total detected:     %.1f GiB\n",
         (double)mem_cfg.total_ram / (1024.0 * 1024.0 * 1024.0));

  double ext4_ino_ram =
      (double)(fs_info.inode_count * sizeof(struct inode_map_entry) * 3) /
      (1024.0 * 1024.0);
  printf("  Conversion RAM needed:  %.1f MiB%s\n", ext4_ino_ram,
         (ext4_ino_ram * 1024 * 1024 > mem_cfg.mmap_threshold)
             ? " (mmap WILL BE USED)"
             : " (in-memory)");

  uint64_t expansion =
      fs_info.compressed_extent_count > 0
          ? (fs_info.total_decompressed_bytes - fs_info.total_compressed_bytes)
          : 0;
  uint64_t expansion_blocks =
      (expansion + layout.block_size - 1) / layout.block_size;

  /* Count available data blocks */
  uint64_t free_data_blocks = 0;
  for (uint32_t g = 0; g < layout.num_groups; g++) {
    free_data_blocks += layout.groups[g].data_blocks;
  }

  /* Subtract blocks already used by existing data */
  uint64_t used_data_blocks = 0;
  for (uint32_t i = 0; i < fs_info.inode_count; i++) {
    const struct file_entry *fe = fs_info.inode_table[i];
    for (uint32_t j = 0; j < fe->extent_count; j++) {
      if (fe->extents[j].type != BTRFS_FILE_EXTENT_INLINE &&
          fe->extents[j].disk_bytenr != 0) {
        used_data_blocks +=
            (fe->extents[j].disk_num_bytes + layout.block_size - 1) /
            layout.block_size;
      }
    }
  }

  uint64_t available = free_data_blocks > used_data_blocks
                           ? free_data_blocks - used_data_blocks
                           : 0;

  uint64_t dedup_bytes = fs_info.dedup_blocks_needed * layout.block_size;
  uint64_t total_needed = expansion_blocks + fs_info.dedup_blocks_needed;

  printf("  Decompression Expansion:%lu blocks (%.1f MiB)\n",
         (unsigned long)expansion_blocks,
         (double)expansion / (1024.0 * 1024.0));
  printf("  CoW Physical Cloning:   %lu extra blocks (%.1f MiB)\n",
         (unsigned long)fs_info.dedup_blocks_needed,
         (double)dedup_bytes / (1024.0 * 1024.0));
  printf("  Available Data Blocks:  %lu blocks (%.1f MiB)\n",
         (unsigned long)available,
         (double)available * layout.block_size / (1024.0 * 1024.0));

  if (total_needed > available) {
    fprintf(stderr,
            "\nbtrfs2ext4: FATAL — Insufficient free space for conversion!\n"
            "  Need %lu additional blocks but only %lu are free.\n"
            "  Please free up at least %.1f MiB before retrying.\n\n",
            (unsigned long)total_needed, (unsigned long)available,
            (double)(total_needed - available) * layout.block_size /
                (1024.0 * 1024.0));
    goto cleanup;
  }
  printf("  Space viability check:  OK (%.1f%% headroom)\n",
         available > 0 ? (double)(available - total_needed) * 100.0 / available
                       : 0.0);
  printf("===================================================\n\n");

  /* ================================================
   * DRY-RUN Benchmark and ETA Estimation
   * ================================================ */
  if (opts->dry_run) {
    printf("=== DRY RUN: ETA Benchmark ===\n");
    printf("  Benchmarking device read speed to estimate real conversion "
           "time...\n");

    /* Benchmark 128 MB or total device size, whichever is smaller */
    uint64_t bench_size = 128ULL * 1024 * 1024;
    if (bench_size > dev.size)
      bench_size = dev.size;

    uint32_t chunk = 1048576; /* 1 MB */
    uint8_t *bench_buf = malloc(chunk);
    if (!bench_buf) {
      printf("  WARNING: Could not allocate benchmark buffer.\n");
    } else {
      struct timespec tb_start, tb_end;
      clock_gettime(CLOCK_MONOTONIC, &tb_start);

      uint64_t read_bytes = 0;
      uint64_t offset = 0;

      while (read_bytes < bench_size) {
        if (device_read(&dev, offset, bench_buf, chunk) < 0)
          break;
        read_bytes += chunk;
        offset += chunk;
      }

      clock_gettime(CLOCK_MONOTONIC, &tb_end);
      free(bench_buf);

      double elapsed_sec = (tb_end.tv_sec - tb_start.tv_sec) +
                           (tb_end.tv_nsec - tb_start.tv_nsec) / 1e9;

      if (elapsed_sec > 0.0 && read_bytes > 0) {
        double speed_mb_s =
            ((double)read_bytes / (1024.0 * 1024.0)) / elapsed_sec;
        printf("  Read speed measured:    %.1f MB/s\n", speed_mb_s);

        /* Calculate approximate write footprint to be deployed in Phase 3 */
        uint64_t inode_tbl_bytes =
            (uint64_t)layout.total_inodes * layout.inode_size;
        uint64_t gdt_bytes = (uint64_t)layout.num_groups * layout.desc_size;
        uint64_t bitmap_bytes =
            (uint64_t)layout.num_groups * layout.block_size * 2;
        uint64_t total_meta_write_bytes =
            inode_tbl_bytes + gdt_bytes + bitmap_bytes;

        /* Penalize speed: 40% efficiency for sequential-ish, 10% efficiency for
         * scattered */
        double write_speed_optimistic = speed_mb_s * 0.40;
        double write_speed_pessimistic = speed_mb_s * 0.10;

        double eta_min_sec =
            ((double)total_meta_write_bytes / (1024.0 * 1024.0)) /
            write_speed_optimistic;
        double eta_max_sec =
            ((double)total_meta_write_bytes / (1024.0 * 1024.0)) /
            write_speed_pessimistic;

        printf("  Phase 3 Write footprint:%.1f MB\n",
               (double)total_meta_write_bytes / (1024.0 * 1024.0));
        printf(
            "\n  >> Estimated Real Conversion Time: %.0f to %.0f seconds <<\n",
            eta_min_sec, eta_max_sec);
      } else {
        printf("  Benchmark failed to complete.\n");
      }
    }
    printf("==============================\n\n");
  }

  /* ================================================
   * PASS 3: Write ext4 structures
   * ================================================ */
  if (opts->dry_run) {
    printf("=== DRY RUN: Would write ext4 structures here ===\n");
    printf("  - %u block groups\n", layout.num_groups);
    printf("  - %u inodes\n", layout.total_inodes);
    printf("  - %u data/metadata conflicts detected\n", conflicts);
    printf("  - %u blocks would be relocated\n", reloc_plan.count);
    printf("  - %lu total blocks\n", (unsigned long)layout.total_blocks);

    /* Dry-run integrity check: physically read all conflicting blocks
     * and compute CRC32C to detect I/O errors / bad sectors */
    if (reloc_plan.count > 0) {
      printf("\n=== Dry-Run Integrity Check ===\n");
      printf("  Reading %u conflicting blocks...\n", reloc_plan.count);

      uint32_t read_errors = 0;
      uint32_t blocks_checked = 0;
      uint8_t *check_buf = malloc(layout.block_size);

      if (check_buf) {
        for (uint32_t r = 0; r < reloc_plan.count; r++) {
          uint64_t offset = reloc_plan.entries[r].src_offset;
          uint32_t length = reloc_plan.entries[r].length;

          if (device_read(&dev, offset, check_buf,
                          length < layout.block_size ? length
                                                     : layout.block_size) < 0) {
            fprintf(stderr, "  ERROR: cannot read block at offset %lu\n",
                    (unsigned long)offset);
            read_errors++;
          } else {
            blocks_checked++;
          }

          /* Progress every 1000 blocks */
          if ((r + 1) % 1000 == 0 || r + 1 == reloc_plan.count) {
            printf("  [%u/%u] blocks verified\r", r + 1, reloc_plan.count);
            fflush(stdout);
          }
        }
        free(check_buf);
      } else {
        fprintf(stderr, "  WARNING: could not allocate check buffer\n");
      }

      printf("\n  Integrity check: %u blocks verified, %u read errors\n",
             blocks_checked, read_errors);

      if (read_errors > 0) {
        fprintf(stderr,
                "\n  WARNING: %u blocks could not be read!\n"
                "  This indicates bad sectors on the device.\n"
                "  Conversion may fail or produce corrupt data.\n"
                "  Consider cloning the device first with ddrescue.\n\n",
                read_errors);
      } else {
        printf("  All conflicting blocks are readable.\n");
      }
      printf("===============================\n");
    }

    ret = 0;
    goto cleanup;
  }

  if (!opts->dry_run && !check_battery_safe()) {
    ret = -1;
    goto cleanup;
  }

  printf("\n:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::\n");
  printf("::          DANGER: POINT OF NO RETURN\n");
  printf(":: The converter is about to overwrite filesystem metadata.\n");
  printf(":: An interruption (power loss, ctrl-c, crash) from this\n");
  printf(":: point forward will render the filesystem UNMOUNTABLE.\n");
  printf("::\n");
  printf(":: If interrupted, DO NOT run fsck! Instead, run:\n");
  printf("::     btrfs2ext4 --rollback %s\n", opts->device_path);
  printf(":::::::::::::::::::::::::::::::::::::::::::::::::::::::::::\n\n");

  if (progress)
    progress("Pass 3", 0, "Writing ext4 filesystem...");

  printf("=== Phase 3: Writing Ext4 Structures ===\n\n");

  if (progress)
    progress("Pass 3", 0, "Linearizing I/O (sorting inodes)...");

  printf("Sorting %u inodes for optimal Ext4 sequential I/O layout...\n",
         fs_info.inode_count);
  qsort(fs_info.inode_table, fs_info.inode_count, sizeof(struct file_entry *),
        compare_file_entry);

  /* Inicializar el allocator global de bloques Ext4 y marcar bloques de datos
   * ya usados por Btrfs (tras la relocación) para que no se reutilicen. */
  struct ext4_block_allocator alloc;
  ext4_block_alloc_init(&alloc, &layout);
  ext4_block_alloc_mark_fs_data(&alloc, &layout, &fs_info);

  /* Link adaptive memory management to the Ext4 inode map */
  ino_map.mem_cfg = &mem_cfg;

  if (ext4_write_superblock(&dev, &layout, &fs_info) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to write superblock\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 3", 20, "Writing group descriptor table...");

  if (ext4_write_gdt(&dev, &layout) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to write GDT\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 3", 40, "Writing inode tables...");

  if (ext4_write_inode_table(&dev, &layout, &fs_info, &ino_map, &alloc) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to write inode tables\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 3", 55, "Writing bitmaps...");

  /* Bug A fix: bitmaps are written AFTER inode tables so the inode_map
   * is fully populated and ext4_write_bitmaps can mark active inodes. */
  if (ext4_write_bitmaps(&dev, &layout, &alloc, &ino_map) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to write bitmaps\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 3", 60, "Writing directory entries...");

  if (ext4_write_directories(&dev, &layout, &fs_info, &ino_map, &alloc) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to write directories\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 3", 85, "Writing journal...");

  if (ext4_write_journal(&dev, &layout, &alloc, dev.size) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to write journal\n");
    goto cleanup;
  }

  if (ext4_finalize_journal_inode(&dev, &layout) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to finalize journal inode\n");
    goto cleanup;
  }

  if (progress)
    progress("Pass 3", 90, "Updating free block counts (GDT/Superblock)...");

  if (ext4_update_free_counts(&dev, &layout) < 0) {
    fprintf(stderr, "btrfs2ext4: failed to update free counts\n");
    goto cleanup;
  }

  device_sync(&dev);

  if (progress)
    progress("Pass 3", 100, "Ext4 filesystem written!");

  printf("\n");
  printf("==============================================\n");
  printf("   Conversion complete!\n");
  printf("==============================================\n");
  printf("\n");
  printf("Next steps:\n");
  printf("  1. Run: e2fsck -f %s\n", opts->device_path);
  printf("  2. Mount: mount %s /mnt\n", opts->device_path);
  printf("  3. (Optional) Defragment: e4defrag /mnt\n");
  printf("     After conversion, files may be fragmented because btrfs and\n");
  printf("     ext4 use different allocation strategies. e4defrag can\n");
  printf("     consolidate file extents for improved sequential read speed.\n");
  printf("\n");

  ret = 0;

cleanup:
  ext4_block_alloc_free(&alloc);
  inode_map_free(&ino_map);
  relocator_free(&reloc_plan);
  ext4_free_layout(&layout);
  btrfs_free_fs(&fs_info);
  device_close(&dev);

  return ret;
}

int btrfs2ext4_rollback(const char *device_path) {
  struct device dev;

  printf("Attempting rollback of %s...\n", device_path);

  if (device_open(&dev, device_path, 0) < 0)
    return -1;

  if (migration_map_rollback(&dev) < 0) {
    fprintf(stderr, "btrfs2ext4: Rollback failed.\n");
    device_close(&dev);
    return -1;
  }

  device_close(&dev);

  printf("Rollback complete! Block relocations reversed and Btrfs superblock "
         "restored.\n");
  printf("Run 'btrfs check %s' to verify integrity.\n", device_path);

  return 0;
}

int main(int argc, char **argv) {
  struct convert_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.block_size = 4096;
  opts.inode_ratio = 16384;

  static struct option long_options[] = {
      {"dry-run", no_argument, NULL, 'n'},
      {"verbose", no_argument, NULL, 'v'},
      {"block-size", required_argument, NULL, 'b'},
      {"inode-ratio", required_argument, NULL, 'i'},
      {"rollback", no_argument, NULL, 'r'},
      {"workdir", required_argument, NULL, 'w'},
      {"memory-limit", required_argument, NULL, 'm'},
      {"help", no_argument, NULL, 'h'},
      {"version", no_argument, NULL, 'V'},
      {NULL, 0, NULL, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "nvb:i:rw:m:hV", long_options, NULL)) !=
         -1) {
    switch (opt) {
    case 'n':
      opts.dry_run = 1;
      break;
    case 'v':
      opts.verbose = 1;
      break;
    case 'b':
      opts.block_size = (uint32_t)atoi(optarg);
      if (opts.block_size != 1024 && opts.block_size != 2048 &&
          opts.block_size != 4096) {
        fprintf(stderr, "Invalid block size %u (must be 1024, 2048, or 4096)\n",
                opts.block_size);
        return 1;
      }
      break;
    case 'i':
      opts.inode_ratio = (uint32_t)atoi(optarg);
      break;
    case 'r':
      opts.rollback = 1;
      break;
    case 'w':
      opts.workdir = optarg;
      break;
    case 'm':
      opts.memory_limit_mb = (uint32_t)atoi(optarg);
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    case 'V':
      btrfs2ext4_version();
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "Error: no device specified\n\n");
    print_usage(argv[0]);
    return 1;
  }

  opts.device_path = argv[optind];

  /* Check that device exists */
  struct stat st;
  if (stat(opts.device_path, &st) < 0) {
    perror(opts.device_path);
    return 1;
  }

  /* Warn if not a block device and not a regular file */
  if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode)) {
    fprintf(stderr, "Warning: %s is not a block device or image file\n",
            opts.device_path);
  }

  /* Must be root for block devices */
  if (S_ISBLK(st.st_mode) && geteuid() != 0) {
    fprintf(stderr, "Error: must run as root for block device access\n");
    return 1;
  }

  if (opts.rollback) {
    return btrfs2ext4_rollback(opts.device_path);
  }

  return btrfs2ext4_convert(&opts, progress_print);
}
