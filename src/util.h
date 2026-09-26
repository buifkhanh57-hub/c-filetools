/*
 * util.h — shared primitives for diskdive: allocation, dynamic buffers,
 * human-readable formatting, paths, colors, JSON escaping and output helpers.
 *
 * diskdive — a professional disk-usage analyzer for POSIX systems.
 * Copyright (c) 2026 Bui Bao Khanh. MIT license (see README.md).
 */
#ifndef DD_UTIL_H
#define DD_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

#define DD_VERSION "1.0.0"
#define DD_PROGNAME "diskdive"

/** Process-wide program name used as prefix for diagnostics. */
extern const char *dd_progname;

/** Install a custom program name (called once from main()). */
void dd_set_progname(const char *name);

/* ------------------------------------------------------------------ */
/* Exit codes (documented contract, see README.md)                     */
/* ------------------------------------------------------------------ */

enum {
    DD_EXIT_OK = 0,      /**< everything went fine                     */
    DD_EXIT_RUNTIME = 1, /**< runtime error (bad path, deletion failed)*/
    DD_EXIT_USAGE = 2    /**< usage error (bad option or command)      */
};

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/** Print "diskdive: <msg>" to stderr. */
void dd_error(const char *fmt, ...);

/** Print "diskdive: warning: <msg>" to stderr. */
void dd_warn(const char *fmt, ...);

/** dd_error() followed by exit(DD_EXIT_RUNTIME). */
void dd_die(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Checked allocation — aborts the process on out-of-memory            */
/* ------------------------------------------------------------------ */

void *dd_xmalloc(size_t n);
void *dd_xcalloc(size_t n, size_t sz);
void *dd_xrealloc(void *p, size_t n);
char *dd_xstrdup(const char *s);

/* ------------------------------------------------------------------ */
/* dd_strbuf — a growable, always NUL-terminated byte buffer           */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;  /**< heap buffer, never NULL after init      */
    size_t len;   /**< current string length (without NUL)     */
    size_t cap;   /**< allocated capacity (>= len + 1)         */
} dd_strbuf;

void dd_sb_init(dd_strbuf *sb);
void dd_sb_free(dd_strbuf *sb);
void dd_sb_reset(dd_strbuf *sb);
/** Ensure capacity for at least @p extra more bytes plus the NUL. */
void dd_sb_reserve(dd_strbuf *sb, size_t extra);
void dd_sb_append(dd_strbuf *sb, const char *s);
void dd_sb_appendn(dd_strbuf *sb, const char *s, size_t n);
void dd_sb_appendch(dd_strbuf *sb, char c);
void dd_sb_appendf(dd_strbuf *sb, const char *fmt, ...);
/** Shrink to @p len bytes (no realloc) and NUL-terminate. */
void dd_sb_truncate(dd_strbuf *sb, size_t len);

/* ------------------------------------------------------------------ */
/* dd_strlist — growable list of owned strings                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} dd_strlist;

void dd_sl_init(dd_strlist *sl);
void dd_sl_add(dd_strlist *sl, const char *s);
/** 1 if @p s is present as an exact entry, else 0. */
int  dd_sl_contains(const dd_strlist *sl, const char *s);
void dd_sl_free(dd_strlist *sl);

/* ------------------------------------------------------------------ */
/* Human-readable sizes and numbers                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    DD_SIZE_BIN = 0, /**< binary units: KiB, MiB, GiB ... (1024-based) */
    DD_SIZE_SI  = 1  /**< decimal units: KB, MB, GB ...   (1000-based) */
} dd_size_mode;

/** Format @p bytes into @p buf ("12.3 MiB"). Returns @p buf. */
const char *dd_human(uint64_t bytes, dd_size_mode mode, char *buf, size_t bufsz);

/**
 * Parse a size with optional suffix: "1024", "10K", "1.5M", "2GiB".
 * Suffixes K/M/G/T/P are 1024-based; a trailing "i" and "B" are ignored.
 * Returns 0 on success and stores the byte count in @p out, -1 on error.
 */
int dd_parse_size(const char *s, uint64_t *out);

/** Group digits with commas: 1234567 → "1,234,567". Returns @p buf. */
const char *dd_comma(uint64_t v, char *buf, size_t bufsz);

/* ------------------------------------------------------------------ */
/* Time formatting                                                     */
/* ------------------------------------------------------------------ */

/** "YYYY-MM-DD HH:MM" in local time. Returns @p buf. */
void dd_fmt_time(int64_t epoch, char *buf, size_t bufsz);

/** Compact relative age: "45s", "3h", "12d", "4mo", "1.2y". Returns buf. */
void dd_fmt_age(int64_t seconds, char *buf, size_t bufsz);

/* ------------------------------------------------------------------ */
/* Path helpers (no fixed-size buffers; callers own the storage)       */
/* ------------------------------------------------------------------ */

/** Final path component; "/" yields "/", plain names yield themselves. */
const char *dd_basename(const char *path);

/** Parent directory of @p path appended to @p out ("a" → "."). */
void dd_dirname(const char *path, dd_strbuf *out);

/**
 * Lowercase extension of @p path without the dot ("report.TAR.GZ" → "gz").
 * Files without an extension (or dotfiles like ".profile") yield "(none)".
 * Returns @p buf.
 */
const char *dd_extension(const char *path, char *buf, size_t bufsz);

/** 1 if @p name is a dot-entry but not "." or ".." itself. */
int dd_name_is_hidden(const char *name);

/* ------------------------------------------------------------------ */
/* Colors                                                              */
/* ------------------------------------------------------------------ */

#define DD_C_RESET   "\033[0m"
#define DD_C_BOLD    "\033[1m"
#define DD_C_DIM     "\033[2m"
#define DD_C_RED     "\033[31m"
#define DD_C_GREEN   "\033[32m"
#define DD_C_YELLOW  "\033[33m"
#define DD_C_BLUE    "\033[34m"
#define DD_C_MAGENTA "\033[35m"
#define DD_C_CYAN    "\033[36m"

enum {
    DD_COLOR_AUTO = 0,      /**< tty + NO_COLOR + TERM heuristics */
    DD_COLOR_FORCE_ON = 1,  /**< --color                          */
    DD_COLOR_FORCE_OFF = 2  /**< --no-color                       */
};

/** Global switch consulted by dd_c(); set via dd_color_apply(). */
extern int dd_color_on;

/** Resolve the color mode (auto / forced) against the environment. */
void dd_color_apply(int mode);

/** Returns @p code when colors are enabled, otherwise "". */
const char *dd_c(const char *code);

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */
/* ------------------------------------------------------------------ */

/* glibc marks fputs/fwrite with warn_unused_result; these wrappers keep
 * -Wall -Wextra silent while preserving the error state in the FILE*. */
#define DD_FPUTS(s, f)          do { if (fputs((s), (f)) == EOF) { } } while (0)
#define DD_FPUTC(c, f)          do { if (fputc((c), (f)) == EOF) { } } while (0)
#define DD_FWRITE(p, sz, n, f)  do { if (fwrite((p), (sz), (n), (f)) < (size_t)(n)) { } } while (0)

/** fprintf() wrapper that tolerates unchecked results (returns count). */
int dd_fprintf(FILE *f, const char *fmt, ...);

/** 1 if the stream has not entered an error state. */
int dd_out_ok(FILE *f);

/* ------------------------------------------------------------------ */
/* Bars and JSON                                                       */
/* ------------------------------------------------------------------ */

/** Append a Unicode progress bar (filled/empty blocks) to @p sb. */
void dd_bar(dd_strbuf *sb, double frac, int width);

/** Print @p s as a quoted, escaped JSON string to @p out. */
void dd_json_quote(FILE *out, const char *s);

#endif /* DD_UTIL_H */
