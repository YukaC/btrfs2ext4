/*
 * inode_writer.c — Ext4 inode table writer
 *
 * Translates btrfs inodes to ext4 inodes and writes them to the inode table.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <endian.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btrfs/btrfs_reader.h"
#include "btrfs/btrfs_structures.h"
#include "btrfs/chunk_tree.h"
#include "btrfs/decompress.h"
#include "device_io.h"
#include "ext4/ext4_metadata_csum.h"
#include "ext4/ext4_planner.h"
#include "ext4/ext4_structures.h"
#include "ext4/ext4_writer.h"
#include "relocator.h"
#include "thread_pool.h"

static int dir_needs_htree(const struct file_entry *dir, uint32_t block_size) {
  uint32_t dir_size = 24;
  for (uint32_t c = 0; c < dir->child_count; c++) {
    uint8_t nl = (uint8_t)dir->children[c].name_len;
    if (nl > 0)
      dir_size += (uint8_t)((8 + nl + 3) & ~3);
  }
  return dir_size > block_size;
}

/* Global decomp pool — Phase 5 Approach C producer-consumer pipeline */
static struct thread_pool *g_decomp_pool = NULL;
#define DECOMP_PIPELINE_DEPTH 4

struct decomp_job {
  struct device *dev;
  struct chunk_map *chunk_map;
  struct file_extent *ext;
  uint32_t block_size;

  uint8_t *decomp_buf;
  uint64_t decomp_len;
  int status;
  int ready; /* 0=in flight, 1=done ok, -1=failed */
  pthread_mutex_t lock;
  pthread_cond_t done;
};

static void decomp_job_init(struct decomp_job *job) {
  memset(job, 0, sizeof(*job));
  job->ready = 0;
  job->status = -1;
  pthread_mutex_init(&job->lock, NULL);
  pthread_cond_init(&job->done, NULL);
}

static void decomp_job_destroy(struct decomp_job *job) {
  pthread_mutex_destroy(&job->lock);
  pthread_cond_destroy(&job->done);
}

static void decomp_worker(void *arg) {
  struct decomp_job *job = arg;
  uint8_t *thread_buf = NULL;
  int st = btrfs_decompress_extent(job->dev, job->chunk_map, job->ext,
                                   job->block_size, &thread_buf,
                                   &job->decomp_len);
  if (st == 0 && thread_buf && job->decomp_len > 0) {
    job->decomp_buf = malloc(job->decomp_len);
    if (job->decomp_buf)
      memcpy(job->decomp_buf, thread_buf, job->decomp_len);
    else
      st = -1;
  }
  pthread_mutex_lock(&job->lock);
  job->status = st;
  job->ready = (st == 0) ? 1 : -1;
  pthread_cond_signal(&job->done);
  pthread_mutex_unlock(&job->lock);
}

static int decomp_job_wait(struct decomp_job *job) {
  pthread_mutex_lock(&job->lock);
  while (job->ready == 0)
    pthread_cond_wait(&job->done, &job->lock);
  int ok = job->ready == 1;
  pthread_mutex_unlock(&job->lock);
  return ok ? 0 : -1;
}

static void decomp_job_submit_async(struct decomp_job *job) {
  if (thread_pool_submit(g_decomp_pool, decomp_worker, job, NULL) < 0)
    decomp_worker(job);
}

/*
 * Write decompressed data for one job via io_uring batch API.
 * Called on the main/I/O thread as each decomp job completes.
 */
static int decomp_job_write_io(struct device *dev, struct decomp_job *job,
                               struct file_extent *ext,
                               struct file_entry *fe_mut, uint32_t e,
                               struct ext4_block_allocator *alloc,
                               const struct ext4_layout *layout,
                               uint32_t block_size) {
  if (decomp_job_wait(job) < 0) {
    fprintf(stderr,
            "btrfs2ext4: failed to decompress_extent for inode %lu\n",
            (unsigned long)fe_mut->ino);
    free(job->decomp_buf);
    return -1;
  }

  uint8_t *decomp_buf = job->decomp_buf;
  uint64_t decomp_len = job->decomp_len;

  uint32_t needed_blocks =
      (uint32_t)((decomp_len + block_size - 1) / block_size);

  struct run {
    uint64_t phys_block;
    uint32_t count;
  };
  struct run *runs = calloc(needed_blocks, sizeof(struct run));
  if (!runs) {
    free(decomp_buf);
    return -1;
  }
  uint32_t num_runs = 0;
  int alloc_failed = 0;

  for (uint32_t b = 0; b < needed_blocks; b++) {
    uint64_t blk = ext4_alloc_block(alloc, layout);
    if (blk == (uint64_t)-1) {
      fprintf(stderr,
              "btrfs2ext4: no space for decompressed block %u (inode %lu)\n",
              b, (unsigned long)fe_mut->ino);
      alloc_failed = 1;
      break;
    }
    if (num_runs > 0 &&
        runs[num_runs - 1].phys_block + runs[num_runs - 1].count == blk) {
      runs[num_runs - 1].count++;
    } else {
      runs[num_runs].phys_block = blk;
      runs[num_runs].count = 1;
      num_runs++;
    }
  }

  device_write_batch_begin(dev);
  uint8_t **run_bufs = calloc(num_runs, sizeof(uint8_t *));
  uint32_t blocks_written = 0;
  for (uint32_t r = 0; r < num_runs && !alloc_failed; r++) {
    uint64_t run_byte_offset = runs[r].phys_block * block_size;
    size_t run_bytes = (size_t)runs[r].count * block_size;
    uint64_t src_offset = (uint64_t)blocks_written * block_size;
    size_t write_len = run_bytes;
    if (src_offset + write_len > decomp_len)
      write_len = (size_t)(decomp_len - src_offset);

    uint8_t *run_buf = calloc(1, run_bytes);
    if (run_buf) {
      if (write_len > 0)
        memcpy(run_buf, decomp_buf + src_offset, write_len);
      device_write_batch_add(dev, run_byte_offset, run_buf, run_bytes);
      if (run_bufs)
        run_bufs[r] = run_buf;
    }
    blocks_written += runs[r].count;
  }
  device_write_batch_submit(dev);
  if (run_bufs) {
    for (uint32_t r = 0; r < num_runs; r++)
      free(run_bufs[r]);
    free(run_bufs);
  }

  if (alloc_failed || num_runs == 0) {
    free(runs);
    free(decomp_buf);
    return -1;
  }

  if (num_runs == 1) {
    ext->disk_bytenr = runs[0].phys_block * block_size;
    ext->disk_num_bytes = (uint64_t)runs[0].count * block_size;
    ext->num_bytes = decomp_len;
    ext->ram_bytes = decomp_len;
    ext->compression = BTRFS_COMPRESS_NONE;
  } else {
    if (fe_mut->extent_count + num_runs - 1 > fe_mut->extent_capacity) {
      fe_mut->extent_capacity = fe_mut->extent_count + num_runs - 1;
      struct file_extent *new_exts =
          realloc(fe_mut->extents,
                  fe_mut->extent_capacity * sizeof(struct file_extent));
      if (!new_exts) {
        free(runs);
        free(decomp_buf);
        return -1;
      }
      fe_mut->extents = new_exts;
      ext = &fe_mut->extents[e];
    }
    if (e + 1 < fe_mut->extent_count) {
      memmove(&fe_mut->extents[e + num_runs], &fe_mut->extents[e + 1],
              (fe_mut->extent_count - e - 1) * sizeof(struct file_extent));
    }
    uint64_t current_file_offset = ext->file_offset;
    uint64_t remaining_decomp_len = decomp_len;
    uint8_t base_type = ext->type;
    for (uint32_t r = 0; r < num_runs; r++) {
      struct file_extent *r_ext = &fe_mut->extents[e + r];
      memset(r_ext, 0, sizeof(struct file_extent));
      r_ext->type = base_type;
      r_ext->compression = BTRFS_COMPRESS_NONE;
      r_ext->disk_bytenr = runs[r].phys_block * block_size;
      r_ext->disk_num_bytes = (uint64_t)runs[r].count * block_size;
      uint64_t run_bytes = (uint64_t)runs[r].count * block_size;
      if (r == num_runs - 1) {
        r_ext->num_bytes = remaining_decomp_len;
        r_ext->ram_bytes = remaining_decomp_len;
      } else {
        r_ext->num_bytes = run_bytes;
        r_ext->ram_bytes = run_bytes;
        remaining_decomp_len -= run_bytes;
      }
      r_ext->file_offset = current_file_offset;
      current_file_offset += r_ext->num_bytes;
    }
    fe_mut->extent_count += (num_runs - 1);
  }

  free(decomp_buf);
  free(runs);
  return (int)(num_runs > 1 ? num_runs - 1 : 0);
}
/* ========================================================================
 * Inode number mapping
 * ======================================================================== */

#define MMAP_THRESHOLD (16 * 1024 * 1024) /* 16 MB */

int inode_map_add(struct inode_map *map, uint64_t btrfs_ino,
                  uint32_t ext4_ino) {
  if (map->count >= map->capacity) {
    uint32_t new_cap = map->capacity ? map->capacity * 2 : 256;
    size_t new_size = new_cap * sizeof(struct inode_map_entry);

    uint64_t threshold =
        map->mem_cfg ? map->mem_cfg->mmap_threshold : (16ULL * 1024 * 1024);

    if (new_size >= threshold) {
      if (map->fd_entries == 0) {
        /* Transition from malloc to mmap */
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s/.btrfs2ext4.tmp.entries",
                 map->mem_cfg ? map->mem_cfg->workdir : ".");

        unlink(tmp_path);
        map->fd_entries =
            open(tmp_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (map->fd_entries < 0)
          return -1;
        if (ftruncate(map->fd_entries, (off_t)new_size) < 0)
          return -1;
        void *p = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       map->fd_entries, 0);
        if (p == MAP_FAILED)
          return -1;

        memcpy(p, map->entries, map->count * sizeof(struct inode_map_entry));
        free(map->entries);
        map->entries = p;
        map->mapped_entries_size = new_size;
      } else {
        /* Expand mmap */
        if (ftruncate(map->fd_entries, (off_t)new_size) < 0)
          return -1;
        void *p = mremap(map->entries, map->mapped_entries_size, new_size,
                         MREMAP_MAYMOVE);
        if (p == MAP_FAILED)
          return -1;
        map->entries = p;
        map->mapped_entries_size = new_size;
      }
    } else {
      /* standard realloc */
      struct inode_map_entry *new_entries =
          realloc(map->capacity ? map->entries : NULL, new_size);
      if (!new_entries) {
        fprintf(stderr, "btrfs2ext4: OOM reallocating inode map\n");
        return -1;
      }
      map->entries = new_entries;
    }
    map->capacity = new_cap;
  }
  map->entries[map->count].btrfs_ino = btrfs_ino;
  map->entries[map->count].ext4_ino = ext4_ino;
  map->count++;

  /* Note: Pass 1 dynamic HT updates removed, we construct it all at once below
   */
  return 0;
}

/*
 * Build the hash table from the existing linear entries.
 * Call once after all inode_map_add() calls are done, before lookups begin.
 */
static void inode_map_build_hash(struct inode_map *map) {
  /* 2× overprovisioned for low collision rate */
  map->ht_size = map->count < 64 ? 128 : map->count * 2;
  size_t hash_size = map->ht_size * sizeof(struct inode_map_entry);

  uint64_t threshold =
      map->mem_cfg ? map->mem_cfg->mmap_threshold : (16ULL * 1024 * 1024);

  if (hash_size >= threshold) {
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.btrfs2ext4.tmp.ht",
             map->mem_cfg ? map->mem_cfg->workdir : ".");

    unlink(tmp_path);
    map->fd_ht = open(tmp_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (map->fd_ht >= 0 && ftruncate(map->fd_ht, (off_t)hash_size) == 0) {
      map->ht_buckets = mmap(NULL, hash_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, map->fd_ht, 0);
      if (map->ht_buckets == MAP_FAILED)
        map->ht_buckets = NULL;
    }
    if (!map->ht_buckets && map->fd_ht >= 0) {
      close(map->fd_ht);
      map->fd_ht = 0;
    } else if (map->ht_buckets) {
      map->mapped_ht_size = hash_size;
    }
  }

  if (!map->ht_buckets) {
    map->ht_buckets = calloc(map->ht_size, sizeof(struct inode_map_entry));
  }

  if (!map->ht_buckets)
    return; /* fallback to linear scan */

  /* Initialize bloom filter if doing mmap hash table to pre-filter disk access
   */
  if (map->fd_ht > 0) {
    map->bloom = calloc(1, sizeof(struct bloom_filter));
    if (map->bloom) {
      bloom_init(map->bloom, map->count);
    }
  }

  for (uint32_t i = 0; i < map->count; i++) {
    uint64_t key = map->entries[i].btrfs_ino;
    if (map->bloom) {
      bloom_add(map->bloom, key);
    }
    uint32_t idx = (uint32_t)(key * 2654435761ULL) % map->ht_size;
    while (map->ht_buckets[idx].ext4_ino != 0) {
      idx = (idx + 1) % map->ht_size;
    }
    map->ht_buckets[idx] = map->entries[i];
  }
}

uint32_t inode_map_lookup(const struct inode_map *map, uint64_t btrfs_ino) {
  /* Pre-filter via Bloom if available (saves HDD page-ins) */
  if (map->bloom && !bloom_test(map->bloom, btrfs_ino))
    return 0;

  /* Use hash table if available (O(1) average) */
  if (map->ht_buckets) {
    uint32_t idx = (uint32_t)(btrfs_ino * 2654435761ULL) % map->ht_size;
    uint32_t start = idx;
    do {
      if (map->ht_buckets[idx].ext4_ino == 0)
        return 0; /* Empty slot = not found */
      if (map->ht_buckets[idx].btrfs_ino == btrfs_ino)
        return map->ht_buckets[idx].ext4_ino;
      idx = (idx + 1) % map->ht_size;
    } while (idx != start);
    return 0;
  }
  /* Fallback: linear scan O(N) */
  for (uint32_t i = 0; i < map->count; i++) {
    if (map->entries[i].btrfs_ino == btrfs_ino)
      return map->entries[i].ext4_ino;
  }
  return 0; /* Not found */
}

void inode_map_free(struct inode_map *map) {
  if (map->bloom) {
    bloom_free(map->bloom);
    free(map->bloom);
    map->bloom = NULL;
  }

  if (map->fd_ht > 0 && map->ht_buckets) {
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.btrfs2ext4.tmp.ht",
             map->mem_cfg ? map->mem_cfg->workdir : ".");

    munmap(map->ht_buckets, map->mapped_ht_size);
    close(map->fd_ht);
    unlink(tmp_path);
  } else {
    free(map->ht_buckets);
  }

  if (map->fd_entries > 0 && map->entries) {
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.btrfs2ext4.tmp.entries",
             map->mem_cfg ? map->mem_cfg->workdir : ".");

    munmap(map->entries, map->mapped_entries_size);
    close(map->fd_entries);
    unlink(tmp_path);
  } else {
    free(map->entries);
  }

  memset(map, 0, sizeof(*map));
}

/* ========================================================================
 * Write inode table for all groups
 * ======================================================================== */

int ext4_write_inode_table(struct device *dev, const struct ext4_layout *layout,
                           const struct btrfs_fs_info *fs_info,
                           struct inode_map *inode_map,
                           struct ext4_block_allocator *alloc) {
  uint32_t block_size = layout->block_size;
  uint32_t inode_size = layout->inode_size;

  printf("Writing inode tables...\n");

  struct ext4_super_block sb;
  if (device_read(dev, EXT4_SUPER_OFFSET, &sb, sizeof(sb)) < 0)
    return -1;

  uint32_t csum_seed = ext4_csum_seed_from_uuid(sb.s_uuid);

  g_decomp_pool = thread_pool_create(4, 1024);

  /* Step 1: Assign ext4 inode numbers to btrfs inodes.
   * Inode 2 = root directory, inodes 1-10 are reserved. */

  /* Map btrfs root dir (inode 256) to ext4 root inode (2) */
  inode_map_add(inode_map, BTRFS_FIRST_FREE_OBJECTID, EXT4_ROOT_INO);

  /* Assign remaining inodes starting from EXT4_GOOD_OLD_FIRST_INO (11) */
  uint32_t next_ino = EXT4_GOOD_OLD_FIRST_INO;
  for (uint32_t i = 0; i < fs_info->inode_count; i++) {
    uint64_t btrfs_ino = fs_info->inode_table[i]->ino;
    if (btrfs_ino == BTRFS_FIRST_FREE_OBJECTID)
      continue; /* Already mapped to inode 2 */
    inode_map_add(inode_map, btrfs_ino, next_ino++);
  }

  printf("  Mapped %u btrfs inodes to ext4 inode numbers\n", inode_map->count);

  /* Build hash table for O(1) lookups from here on */
  inode_map_build_hash(inode_map);

  /* Build auxiliar mapping Ext4→Btrfs para lookups O(1) en el bucle
   * principal de escritura (evita O(N^2)). Tamaño = total_inodes+1
   * porque los inodos empiezan en 1. */
  uint64_t max_ino = layout->total_inodes + 1ULL;
  uint64_t *btrfs_for_ext4 = calloc(max_ino, sizeof(uint64_t));
  if (!btrfs_for_ext4)
    return -1;
  for (uint32_t i = 0; i < inode_map->count; i++) {
    uint32_t e = inode_map->entries[i].ext4_ino;
    if (e > 0 && (uint64_t)e < max_ino)
      btrfs_for_ext4[e] = inode_map->entries[i].btrfs_ino;
  }

  /* Step 2: For each block group, write the inode table */
  for (uint32_t g = 0; g < layout->num_groups; g++) {
    const struct ext4_bg_layout *bg = &layout->groups[g];
    uint32_t table_bytes = layout->inodes_per_group * inode_size;
    uint8_t *table_buf = calloc(1, table_bytes);
    if (!table_buf)
      return -1;

    uint32_t ino_start = g * layout->inodes_per_group + 1;
    uint32_t ino_end = ino_start + layout->inodes_per_group;

    for (uint32_t ino = ino_start; ino < ino_end; ino++) {
      /* Find the btrfs file entry for this ext4 inode (O(1) lookup) */
      uint64_t btrfs_ino = (ino < max_ino) ? btrfs_for_ext4[ino] : 0;
      if (btrfs_ino == 0) {
        /* Special handling for reserved inodes */
        if (ino == EXT4_ROOT_INO) {
          btrfs_ino = BTRFS_FIRST_FREE_OBJECTID;
        } else if (ino == EXT4_JOURNAL_INO) {
          /* Journal inode — handled separately below */
          uint32_t jnl_blocks = ext4_journal_block_count();
          uint64_t jnl_start = ext4_journal_start_block();

          if (jnl_blocks > 0 && jnl_start > 0) {
            struct ext4_inode *jnl_inode =
                (struct ext4_inode *)(table_buf +
                                      (ino - ino_start) * inode_size);
            jnl_inode->i_mode = htole16(S_IFREG | 0600);
            uint64_t jnl_size = (uint64_t)jnl_blocks * block_size;
            jnl_inode->i_size_lo = htole32((uint32_t)(jnl_size & 0xFFFFFFFF));
            jnl_inode->i_size_high = htole32((uint32_t)(jnl_size >> 32));
            jnl_inode->i_links_count = htole16(1);
            uint64_t jnl_sectors = (jnl_size + 511) / 512;
            jnl_inode->i_blocks_lo =
                htole32((uint32_t)(jnl_sectors & 0xFFFFFFFF));
            jnl_inode->i_blocks_high = htole16((uint16_t)(jnl_sectors >> 32));
            jnl_inode->i_flags |= htole32(EXT4_EXTENTS_FL);
            jnl_inode->i_extra_isize = htole16(32);
            jnl_inode->i_generation = htole32(1);

            /* Build extent tree for journal (single extent) */
            struct ext4_extent_header *jeh =
                (struct ext4_extent_header *)jnl_inode->i_block;
            jeh->eh_magic = htole16(EXT4_EXT_MAGIC);
            jeh->eh_entries = htole16(1);
            jeh->eh_max = htole16(4);
            jeh->eh_depth = htole16(0);
            jeh->eh_generation = htole32(0);

            struct ext4_extent *jext =
                (struct ext4_extent *)((uint8_t *)jnl_inode->i_block +
                                       sizeof(struct ext4_extent_header));
            jext->ee_block = htole32(0);
            jext->ee_len =
                htole16(jnl_blocks > 32768 ? 32768 : (uint16_t)jnl_blocks);
            jext->ee_start_lo = htole32((uint32_t)(jnl_start & 0xFFFFFFFF));
            jext->ee_start_hi = htole16((uint16_t)(jnl_start >> 32));
            ext4_inode_set_checksum_ino(csum_seed, jnl_inode, inode_size, ino);
          }
          continue;
        } else {
          continue; /* Unused inode */
        }
      }

      const struct file_entry *fe =
          btrfs_find_inode((struct btrfs_fs_info *)fs_info, btrfs_ino);
      if (!fe)
        continue;

      /* Calculate position in table buffer */
      uint32_t local_ino = ino - ino_start;
      struct ext4_inode *ext_inode =
          (struct ext4_inode *)(table_buf + local_ino * inode_size);

      /* Translate btrfs inode to ext4 */
      ext_inode->i_mode = htole16((uint16_t)fe->mode);
      ext_inode->i_uid = htole16((uint16_t)(fe->uid & 0xFFFF));
      ext_inode->i_uid_high = htole16((uint16_t)(fe->uid >> 16));
      ext_inode->i_gid = htole16((uint16_t)(fe->gid & 0xFFFF));
      ext_inode->i_gid_high = htole16((uint16_t)(fe->gid >> 16));
      ext_inode->i_links_count = htole16((uint16_t)fe->nlink);

      uint64_t size = fe->size;
      ext_inode->i_size_lo = htole32((uint32_t)(size & 0xFFFFFFFF));
      ext_inode->i_size_high = htole32((uint32_t)(size >> 32));

      /* Timestamps */
      ext_inode->i_atime = htole32((uint32_t)fe->atime_sec);
      ext_inode->i_ctime = htole32((uint32_t)fe->ctime_sec);
      ext_inode->i_mtime = htole32((uint32_t)fe->mtime_sec);
      ext_inode->i_crtime = htole32((uint32_t)fe->crtime_sec);

      /* Nanosecond precision in extra fields */
      ext_inode->i_atime_extra =
          htole32(((uint32_t)fe->atime_nsec << 2) |
                  ((uint32_t)((fe->atime_sec >> 32) & 0x3)));
      ext_inode->i_mtime_extra =
          htole32(((uint32_t)fe->mtime_nsec << 2) |
                  ((uint32_t)((fe->mtime_sec >> 32) & 0x3)));
      ext_inode->i_ctime_extra =
          htole32(((uint32_t)fe->ctime_nsec << 2) |
                  ((uint32_t)((fe->ctime_sec >> 32) & 0x3)));
      ext_inode->i_crtime_extra =
          htole32(((uint32_t)fe->crtime_nsec << 2) |
                  ((uint32_t)((fe->crtime_sec >> 32) & 0x3)));

      /* Extra inode size (256-128 = 128, but actual extra = 32 for timestamps)
       */
      ext_inode->i_extra_isize = htole16(32);

      /* Blocks count (in 512-byte sectors) */
      uint64_t blocks_512 = (size + 511) / 512;
      ext_inode->i_blocks_lo = htole32((uint32_t)(blocks_512 & 0xFFFFFFFF));
      ext_inode->i_blocks_high = htole16((uint16_t)(blocks_512 >> 32));

      /* Decompress compressed extents and rewrite to new blocks */
      if (S_ISREG(fe->mode) && fe->extent_count > 0) {
        struct file_entry *fe_mut = (struct file_entry *)fe;
        int has_compressed = 0;
        for (uint32_t e = 0; e < fe_mut->extent_count; e++) {
          if (fe_mut->extents[e].compression != BTRFS_COMPRESS_NONE &&
              fe_mut->extents[e].type != BTRFS_FILE_EXTENT_INLINE &&
              fe_mut->extents[e].type != BTRFS_FILE_EXTENT_PREALLOC &&
              fe_mut->extents[e].disk_bytenr != 0) {
            has_compressed = 1;
            break;
          }
        }

        if (has_compressed) {
          /* Phase 5 Approach C: producer-consumer pipeline — pool workers
           * decompress while main thread batches io_uring writes as jobs
           * complete (rolling window depth = DECOMP_PIPELINE_DEPTH). */
          struct decomp_job *jobs =
              calloc(fe_mut->extent_count, sizeof(struct decomp_job));
          uint32_t *cidx = calloc(fe_mut->extent_count, sizeof(uint32_t));
          if (!jobs || !cidx) {
            free(jobs);
            free(cidx);
            continue;
          }

          uint32_t n_comp = 0;
          for (uint32_t e = 0; e < fe_mut->extent_count; e++) {
            struct file_extent *ext = &fe_mut->extents[e];
            if (ext->compression == BTRFS_COMPRESS_NONE ||
                ext->type == BTRFS_FILE_EXTENT_INLINE ||
                ext->type == BTRFS_FILE_EXTENT_PREALLOC ||
                ext->disk_bytenr == 0)
              continue;
            decomp_job_init(&jobs[e]);
            cidx[n_comp++] = e;
          }

          uint32_t next_submit = 0;
          uint32_t next_write = 0;
          while (next_write < n_comp) {
            while (next_submit < n_comp &&
                   next_submit < next_write + DECOMP_PIPELINE_DEPTH) {
              uint32_t ei = cidx[next_submit];
              jobs[ei].dev = dev;
              jobs[ei].chunk_map = fs_info->chunk_map;
              jobs[ei].ext = &fe_mut->extents[ei];
              jobs[ei].block_size = block_size;
              decomp_job_submit_async(&jobs[ei]);
              next_submit++;
            }

            uint32_t wi = cidx[next_write];
            int added = decomp_job_write_io(
                dev, &jobs[wi], &fe_mut->extents[wi], fe_mut, wi, alloc,
                layout, block_size);
            decomp_job_destroy(&jobs[wi]);
            if (added > 0) {
              for (uint32_t j = next_write + 1; j < n_comp; j++) {
                if (cidx[j] > wi)
                  cidx[j] += (uint32_t)added;
              }
            }
            next_write++;
          }

          free(cidx);
          free(jobs);
        }

                /* Check if we can store it as Native Inline Data (Phase 5) */
        int stored_inline = 0;
        if (fe->extent_count == 1 &&
            fe->extents[0].type == BTRFS_FILE_EXTENT_INLINE &&
            fe->extents[0].inline_data_len > 0) {
          size_t inline_len = fe->extents[0].inline_data_len;
          size_t max_inline_len = 60;
          if (layout->inode_size > 128) {
            /* 128 (extra space) - 32 (timestamp extra) - 4 (xattr magic) -
             * xattr header overhead */
            max_inline_len += (layout->inode_size - 128 - 32 -
                               sizeof(struct ext4_xattr_ibody_header) -
                               sizeof(struct ext4_xattr_entry));
          }

          if (inline_len <= max_inline_len) {
            ext_inode->i_flags |= htole32(EXT4_INLINE_DATA_FL);
            size_t iblock_len = inline_len < 60 ? inline_len : 60;
            memcpy(ext_inode->i_block, fe->extents[0].inline_data, iblock_len);

            if (inline_len > 60) {
              /* Store remainder in extra inode space as system.data xattr */
              uint8_t *extra = (uint8_t *)ext_inode + 128 +
                               32; /* After basic extra fields */
              struct ext4_xattr_ibody_header *xhdr =
                  (struct ext4_xattr_ibody_header *)extra;
              xhdr->h_magic = htole32(EXT4_XATTR_MAGIC);

              struct ext4_xattr_entry *xentry =
                  (struct ext4_xattr_entry
                       *)(extra + sizeof(struct ext4_xattr_ibody_header));
              xentry->e_name_len = 4; /* "data" */
              xentry->e_name_index = EXT4_XATTR_INDEX_SYSTEM;
              xentry->e_value_size = htole32((uint32_t)(inline_len - 60));
              xentry->e_value_offs =
                  htole16((uint16_t)(sizeof(struct ext4_xattr_ibody_header) +
                                     sizeof(struct ext4_xattr_entry) +
                                     8 /* padded name */));
              xentry->e_value_block = 0;
              xentry->e_hash = 0;
              memcpy(xentry->e_name, "data\0\0\0\0",
                     8); /* padded to 4-byte boundary */

              uint8_t *xval =
                  (uint8_t *)xentry + sizeof(struct ext4_xattr_entry) + 8;
              memcpy(xval, fe->extents[0].inline_data + 60, inline_len - 60);

              /* Mark end of xattr entries */
              uint32_t *xend =
                  (uint32_t *)(xval + ((inline_len - 60 + 3) & ~3));
              *xend = 0;
            }
            stored_inline = 1;
          }
        }

        if (!stored_inline) {
          /* Build extent tree for regular files (supports multi-level) */
          ext4_build_extent_tree(alloc, dev, ext_inode, fe, fs_info->chunk_map,
                                 layout);
        }
      } else if (S_ISDIR(fe->mode)) {
        uint32_t dir_flags = EXT4_EXTENTS_FL;
        if (dir_needs_htree(fe, block_size))
          dir_flags |= EXT4_INDEX_FL;
        ext_inode->i_flags |= htole32(dir_flags);
        struct ext4_extent_header *eh =
            (struct ext4_extent_header *)ext_inode->i_block;
        eh->eh_magic = htole16(EXT4_EXT_MAGIC);
        eh->eh_entries = htole16(0);
        eh->eh_max = htole16(4);
        eh->eh_depth = htole16(0);
        eh->eh_generation = htole32(0);
      } else if (S_ISLNK(fe->mode) && fe->symlink_target) {
        /* Symlinks: store target in i_block if short enough (<60 bytes),
         * otherwise need an extent-based data block */
        size_t target_len = strlen(fe->symlink_target);
        if (target_len < 60) {
          /* Fast symlink: target stored directly in i_block */
          memcpy(ext_inode->i_block, fe->symlink_target, target_len);
        } else {
          /* Security check: Linux limits symlinks to PATH_MAX.
           * Prevent heap buffer overflow if Btrfs inline extent is maliciously
           * huge. */
          if (target_len >= block_size) {
            target_len = block_size - 1;
          }

          /* Long symlink: allocate a data block and store target there */
          uint64_t sym_block = ext4_alloc_block(alloc, layout);
          if (sym_block != (uint64_t)-1) {
            uint8_t *sym_buf = calloc(1, block_size);
            if (sym_buf) {
              memcpy(sym_buf, fe->symlink_target, target_len);
              device_write(dev, sym_block * block_size, sym_buf, block_size);
              free(sym_buf);

              /* Build inline extent pointing to the data block */
              struct ext4_extent_header *eh =
                  (struct ext4_extent_header *)ext_inode->i_block;
              eh->eh_magic = htole16(EXT4_EXT_MAGIC);
              eh->eh_entries = htole16(1);
              eh->eh_max = htole16(4);
              eh->eh_depth = htole16(0);
              struct ext4_extent *ext =
                  (struct ext4_extent *)((uint8_t *)ext_inode->i_block +
                                         sizeof(struct ext4_extent_header));
              ext->ee_block = htole32(0);
              ext->ee_len = htole16(1);
              ext->ee_start_lo = htole32((uint32_t)(sym_block & 0xFFFFFFFF));
              ext->ee_start_hi = htole16((uint16_t)(sym_block >> 32));
              ext_inode->i_flags |= htole32(EXT4_EXTENTS_FL);
            }
          }
        }
      } else if (S_ISCHR(fe->mode) || S_ISBLK(fe->mode)) {
        /* Device nodes: store rdev in i_block */
        uint32_t major = (uint32_t)(fe->rdev >> 8) & 0xFFF;
        uint32_t minor = (uint32_t)(fe->rdev & 0xFF) |
                         ((uint32_t)(fe->rdev >> 12) & 0xFFF00);
        /* Old encoding in i_block[0] */
        ((uint32_t *)ext_inode->i_block)[0] =
            htole32((major << 8) | (minor & 0xFF));
        /* New encoding in i_block[1] */
        ((uint32_t *)ext_inode->i_block)[1] = htole32((major << 20) | minor);
      }

      /* Write security xattrs (Phase 6) */
      if (fe->xattrs && !(ext_inode->i_flags & htole32(EXT4_INLINE_DATA_FL))) {
        /* Only write if we haven't already used the ibody for inline data */
        if (layout->inode_size >
            128 + 32 + sizeof(struct ext4_xattr_ibody_header)) {
          uint8_t *extra = (uint8_t *)ext_inode + 128 + 32;
          struct ext4_xattr_ibody_header *xhdr =
              (struct ext4_xattr_ibody_header *)extra;
          xhdr->h_magic = htole32(EXT4_XATTR_MAGIC);

          struct ext4_xattr_entry *xentry =
              (struct ext4_xattr_entry *)(extra +
                                          sizeof(
                                              struct ext4_xattr_ibody_header));
          uint8_t *xval_area = extra + layout->inode_size - 128 -
                               32; /* Start values from end of inode */
          int space_left = layout->inode_size - 128 - 32 -
                           sizeof(struct ext4_xattr_ibody_header) -
                           4; /* -4 for end null eq */

          struct xattr_entry *xa = fe->xattrs;
          while (xa) {
            /* Determine name index (security vs system vs user) */
            uint8_t name_index = 0; /* EXT4_XATTR_INDEX_USER default */
            const char *name_rem = xa->name;
            if (strncmp(xa->name, "security.", 9) == 0) {
              name_index = EXT4_XATTR_INDEX_SECURITY;
              name_rem += 9;
            } else if (strncmp(xa->name, "system.", 7) == 0) {
              name_index = EXT4_XATTR_INDEX_SYSTEM;
              name_rem += 7;
            } else if (strncmp(xa->name, "user.", 5) == 0) {
              name_index = 1; /* EXT4_XATTR_INDEX_USER */
              name_rem += 5;
            }

            size_t rem_len = strlen(name_rem);
            size_t name_pad = (rem_len + 3) & ~3;
            size_t val_pad = (xa->value_len + 3) & ~3;
            size_t entry_size = sizeof(struct ext4_xattr_entry) + name_pad;

            /* Check for integer overflow */
            if (xa->value_len > 4096 || entry_size + val_pad > 4096) {
              xa = xa->next;
              continue;
            }

            if (space_left >= (int)(entry_size + val_pad)) {
              xentry->e_name_len = rem_len;
              xentry->e_name_index = name_index;
              xentry->e_value_block = 0;
              xentry->e_value_size = htole32(xa->value_len);
              xval_area -= val_pad;
              xentry->e_value_offs = htole16((uint16_t)(xval_area - extra));
              xentry->e_hash = 0;

              memset(xentry->e_name, 0, name_pad);
              memcpy(xentry->e_name, name_rem, rem_len);

              if (xa->value_len > 0) {
                memcpy(xval_area, xa->value, xa->value_len);
              }

              space_left -= (entry_size + val_pad);
              xentry =
                  (struct ext4_xattr_entry *)((uint8_t *)xentry + entry_size);
            }
            xa = xa->next;
          }
          /* Terminate entry list */
          *(uint32_t *)xentry = 0;
        }
      }

      ext_inode->i_generation = htole32(1); /* Generation number */
      ext4_inode_set_checksum_ino(csum_seed, ext_inode, inode_size, ino);
    }

    /* Write the inode table for this group */
    uint64_t table_offset = bg->inode_table_start * block_size;
    if (device_write(dev, table_offset, table_buf, table_bytes) < 0) {
      free(table_buf);
      free(btrfs_for_ext4);
      return -1;
    }

    free(table_buf);
  }

  printf("  Inode tables written\n");
  free(btrfs_for_ext4);

  if (g_decomp_pool) {
    thread_pool_destroy(g_decomp_pool);
    g_decomp_pool = NULL;
  }

  return 0;
}
