/*
 * term_ui.h — Compact TTY output helpers for btrfs2ext4
 */

#ifndef TERM_UI_H
#define TERM_UI_H

#include <stdint.h>

void term_ui_init(void);
void term_ui_set_verbose(int verbose);
int term_ui_is_verbose(void);
int term_ui_use_color(void);

void term_banner(const char *version);
void term_kv(const char *key, const char *fmt, ...);
void term_ok(const char *fmt, ...);
void term_warn(const char *fmt, ...);
void term_progress(const char *phase, uint32_t percent, const char *detail);

/* Verbose-only diagnostic dumps (no-op when !verbose). */
void term_log_debug(const char *fmt, ...);

#endif /* TERM_UI_H */
