/*
 * btrfs2ext4.h — Main conversion API
 */

#ifndef BTRFS2EXT4_H
#define BTRFS2EXT4_H

#include <stdint.h>

/* Conversion options */
struct convert_options {
  const char *device_path;
  const char *workdir;      /* --workdir: path for temp files (default: pwd) */
  int dry_run;              /* 1 = simulate only, don't write */
  int verbose;              /* 1 = detailed output */
  int rollback;             /* 1 = rollback a previous conversion */
  int no_journal;           /* 1 = skip crash-recovery journal */
  uint32_t block_size;      /* ext4 block size (default 4096) */
  uint32_t inode_ratio;     /* bytes per inode (default 16384) */
  uint32_t memory_limit_mb; /* --memory-limit: max RAM MB (0=auto) */
};

/* Conversion progress callback */
typedef void (*progress_callback)(const char *phase, uint32_t percent,
                                  const char *detail);

/*
 * Perform the in-place btrfs → ext4 conversion.
 *
 * This is the main entry point that orchestrates all three passes:
 *   Pass 1: Read btrfs metadata
 *   Pass 2: Plan ext4 layout + relocate conflicting blocks
 *   Pass 3: Write ext4 structures
 *
 * Returns 0 on success, -1 on error.
 */
int btrfs2ext4_convert(const struct convert_options *opts,
                       progress_callback progress);

/*
 * Rollback a previous conversion.
 * Restores the btrfs superblock from backup.
 * Returns 0 on success, -1 on error.
 */
int btrfs2ext4_rollback(const char *device_path);

/*
 * Print version information.
 */
void btrfs2ext4_version(void);

#endif /* BTRFS2EXT4_H */
