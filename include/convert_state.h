/*
 * convert_state.h — Pass 3 conversion progress footer (Phase 3.5)
 */
#ifndef CONVERT_STATE_H
#define CONVERT_STATE_H

#include <stdint.h>

#define CONVERT_STATE_MAGIC "B2E4CVST"
#define CONVERT_STATE_SIZE 64

#define CONVERT_STATE_PHASE_IDLE 0
#define CONVERT_STATE_PHASE_PASS2_DONE 2
#define CONVERT_STATE_PHASE_PASS3 3

#define CONVERT_STATE_STEP_SUPERBLOCK 0x01
#define CONVERT_STATE_STEP_GDT 0x02
#define CONVERT_STATE_STEP_INODES 0x04
#define CONVERT_STATE_STEP_DIRS 0x08
#define CONVERT_STATE_STEP_JOURNAL 0x10
#define CONVERT_STATE_STEP_BITMAPS 0x20
#define CONVERT_STATE_STEP_FREE_COUNTS 0x40
#define CONVERT_STATE_STEP_COMPLETE 0x80

struct convert_state_footer {
  char magic[8];
  uint32_t phase;
  uint32_t pass3_step_mask;
  uint64_t timestamp;
  uint32_t crc32;
  uint32_t padding[9];
};

struct device;

int convert_state_compute_offset(struct device *dev, uint64_t *offset_out);
int convert_state_present(struct device *dev);
int convert_state_load(struct device *dev, struct convert_state_footer *out);
int convert_state_save(struct device *dev, uint32_t phase, uint32_t pass3_step_mask);
int convert_state_set_step(struct device *dev, uint32_t step_bit);
int convert_state_clear(struct device *dev);
int convert_state_emergency_recover(struct device *dev);

#endif /* CONVERT_STATE_H */
