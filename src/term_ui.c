/*
 * term_ui.c — Compact TTY output helpers
 */

#include "term_ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN "\033[36m"
#define ANSI_RED "\033[31m"

static int g_color = 0;
static int g_unicode = 0;
static int g_verbose = 0;
static int g_initialized = 0;

static int env_truthy(const char *val) {
  if (!val || !*val)
    return 0;
  return !(strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0 ||
           strcasecmp(val, "no") == 0);
}

void term_ui_init(void) {
  g_initialized = 1;
  g_color = 0;
  g_unicode = 0;

  if (!isatty(STDOUT_FILENO))
    return;
  if (getenv("NO_COLOR") != NULL)
    return;
  const char *force = getenv("FORCE_COLOR");
  if (force && !env_truthy(force))
    return;

  g_color = 1;
  const char *lang = getenv("LANG");
  const char *lc = getenv("LC_ALL");
  const char *ctype = getenv("LC_CTYPE");
  if ((lang && strstr(lang, "UTF-8")) || (lang && strstr(lang, "utf8")) ||
      (lc && strstr(lc, "UTF-8")) || (lc && strstr(lc, "utf8")) ||
      (ctype && strstr(ctype, "UTF-8")) || (ctype && strstr(ctype, "utf8")))
    g_unicode = 1;
}

void term_ui_set_verbose(int verbose) { g_verbose = verbose ? 1 : 0; }

int term_ui_is_verbose(void) { return g_verbose; }

int term_ui_use_color(void) {
  if (!g_initialized)
    term_ui_init();
  return g_color;
}

void term_banner(const char *version) {
  if (!g_initialized)
    term_ui_init();
  if (g_color)
    printf("%sbtrfs2ext4%s  %s\n", ANSI_BOLD ANSI_CYAN, ANSI_RESET, version);
  else
    printf("btrfs2ext4  %s\n", version);
  if (g_color)
    printf("%sIn-place Btrfs → Ext4%s\n\n", ANSI_DIM, ANSI_RESET);
  else
    printf("In-place Btrfs -> Ext4\n\n");
}

void term_kv(const char *key, const char *fmt, ...) {
  va_list ap;
  if (g_color)
    printf("  %s%-10s%s", ANSI_DIM, key, ANSI_RESET);
  else
    printf("  %-10s", key);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

void term_ok(const char *fmt, ...) {
  va_list ap;
  if (g_color)
    printf("%s", ANSI_GREEN);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  if (g_color)
    printf("%s", ANSI_RESET);
  printf("\n");
}

void term_warn(const char *fmt, ...) {
  va_list ap;
  if (g_color)
    fprintf(stderr, "%s", ANSI_YELLOW);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  if (g_color)
    fprintf(stderr, "%s", ANSI_RESET);
  fprintf(stderr, "\n");
}

void term_log_debug(const char *fmt, ...) {
  if (!g_verbose)
    return;
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

void term_progress(const char *phase, uint32_t percent,
                   const char *detail) {
  static struct timespec start_time;
  static int started = 0;
  static const char *last_phase = NULL;
  static int last_line_len = 0;

  if (!g_initialized)
    term_ui_init();

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  if (!started || !last_phase || strcmp(phase, last_phase) != 0) {
    if (started && last_line_len > 0 && percent < 100)
      printf("\n");
    start_time = now;
    started = 1;
    last_phase = phase;
    last_line_len = 0;
  }

  char bar[64];
  uint32_t filled = percent / 5;
  if (filled > 20)
    filled = 20;

  if (g_unicode) {
    char *p = bar;
    for (uint32_t i = 0; i < 20; i++) {
      const char *ch = (i < filled) ? "█" : "░";
      size_t n = strlen(ch);
      memcpy(p, ch, n);
      p += n;
    }
    *p = '\0';
  } else {
    for (uint32_t i = 0; i < 20; i++)
      bar[i] = (i < filled) ? '#' : '-';
    bar[20] = '\0';
  }

  double elapsed = (double)(now.tv_sec - start_time.tv_sec) +
                   (double)(now.tv_nsec - start_time.tv_nsec) / 1e9;
  char eta_buf[32] = "";
  if (percent > 0 && percent < 100 && elapsed > 1.0) {
    double total_est = elapsed * 100.0 / (double)percent;
    double remaining = total_est - elapsed;
    if (remaining > 3600)
      snprintf(eta_buf, sizeof(eta_buf), " ETA %.0fh%.0fm", remaining / 3600,
               fmod(remaining, 3600) / 60);
    else if (remaining > 60)
      snprintf(eta_buf, sizeof(eta_buf), " ETA %.0fm%.0fs", remaining / 60,
               fmod(remaining, 60));
    else
      snprintf(eta_buf, sizeof(eta_buf), " ETA %.0fs", remaining);
  }

  char line[512];
  int n;
  if (g_color)
    n = snprintf(line, sizeof(line),
                 "  %s%-8s%s [%s%s%s] %3u%%%s %s", ANSI_BOLD, phase,
                 ANSI_RESET, g_unicode ? ANSI_CYAN : "", bar,
                 g_unicode ? ANSI_RESET : "", percent, eta_buf,
                 detail ? detail : "");
  else
    n = snprintf(line, sizeof(line), "  %-8s [%s] %3u%%%s %s", phase, bar,
                 percent, eta_buf, detail ? detail : "");
  if (n < 0)
    n = 0;
  if ((size_t)n >= sizeof(line))
    n = (int)sizeof(line) - 1;

  printf("\r%s", line);
  if (n < last_line_len) {
    int pad = last_line_len - n;
    while (pad-- > 0)
      putchar(' ');
    printf("\r%s", line);
  }
  last_line_len = n;

  if (percent >= 100) {
    printf("\n");
    last_line_len = 0;
  }
  fflush(stdout);
}
