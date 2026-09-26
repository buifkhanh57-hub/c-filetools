/*
 * util.c — implementation of the shared diskdive primitives.
 *
 * Everything here is deliberately allocation-checked and leak-free:
 * the tool is a short-lived CLI, but correctness still matters.
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const char *dd_progname = DD_PROGNAME;
int dd_color_on = 0;

void dd_set_progname(const char *name)
{
    dd_progname = (name && *name) ? name : DD_PROGNAME;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

static void vdiag(const char *prefix, const char *fmt, va_list ap)
{
    fflush(stdout);
    DD_FPUTS(dd_progname, stderr);
    DD_FPUTS(": ", stderr);
    if (prefix) {
        DD_FPUTS(prefix, stderr);
    }
    if (vfprintf(stderr, fmt, ap) < 0) {
        /* stream in trouble; nothing sensible left to do */
    }
    DD_FPUTC('\n', stderr);
}

void dd_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vdiag("", fmt, ap);
    va_end(ap);
}

void dd_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vdiag("warning: ", fmt, ap);
    va_end(ap);
}

void dd_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vdiag("", fmt, ap);
    va_end(ap);
    exit(DD_EXIT_RUNTIME);
}

/* ------------------------------------------------------------------ */
/* Checked allocation                                                  */
/* ------------------------------------------------------------------ */

static void oom(void)
{
    dd_die("out of memory");
}

void *dd_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        oom();
    }
    return p;
}

void *dd_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        oom();
    }
    return p;
}

void *dd_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        oom();
    }
    return q;
}

char *dd_xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (!s) {
        return NULL;
    }
    n = strlen(s) + 1;
    p = dd_xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* dd_strbuf                                                           */
/* ------------------------------------------------------------------ */

#define DD_SB_MIN_CAP 64

void dd_sb_init(dd_strbuf *sb)
{
    sb->cap = DD_SB_MIN_CAP;
    sb->len = 0;
    sb->data = dd_xmalloc(sb->cap);
    sb->data[0] = '\0';
}

void dd_sb_free(dd_strbuf *sb)
{
    if (sb && sb->data) {
        free(sb->data);
        sb->data = NULL;
    }
    if (sb) {
        sb->len = 0;
        sb->cap = 0;
    }
}

void dd_sb_reset(dd_strbuf *sb)
{
    sb->len = 0;
    sb->data[0] = '\0';
}

void dd_sb_reserve(dd_strbuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) {
        return;
    }
    while (sb->cap < need) {
        sb->cap *= 2;
    }
    sb->data = dd_xrealloc(sb->data, sb->cap);
}

void dd_sb_appendn(dd_strbuf *sb, const char *s, size_t n)
{
    if (n == 0) {
        return;
    }
    dd_sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void dd_sb_append(dd_strbuf *sb, const char *s)
{
    if (s) {
        dd_sb_appendn(sb, s, strlen(s));
    }
}

void dd_sb_appendch(dd_strbuf *sb, char c)
{
    dd_sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void dd_sb_appendf(dd_strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t free_bytes;

    /* first try to write into the existing spare capacity */
    free_bytes = sb->cap - sb->len;
    va_start(ap, fmt);
    n = vsnprintf(sb->data + sb->len, free_bytes, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return; /* encoding error: give up silently but stay consistent */
    }
    if ((size_t)n < free_bytes) {
        sb->len += (size_t)n;
        return;
    }
    /* not enough room: reserve and try again */
    dd_sb_reserve(sb, (size_t)n);
    va_start(ap, fmt);
    n = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb->len += (size_t)n;
    }
}

void dd_sb_truncate(dd_strbuf *sb, size_t len)
{
    if (len > sb->len) {
        return;
    }
    sb->len = len;
    sb->data[sb->len] = '\0';
}

/* ------------------------------------------------------------------ */
/* dd_strlist                                                          */
/* ------------------------------------------------------------------ */

void dd_sl_init(dd_strlist *sl)
{
    sl->items = NULL;
    sl->len = 0;
    sl->cap = 0;
}

void dd_sl_add(dd_strlist *sl, const char *s)
{
    if (sl->len == sl->cap) {
        sl->cap = sl->cap ? sl->cap * 2 : 8;
        sl->items = dd_xrealloc(sl->items, sl->cap * sizeof(*sl->items));
    }
    sl->items[sl->len++] = dd_xstrdup(s);
}

int dd_sl_contains(const dd_strlist *sl, const char *s)
{
    size_t i;
    for (i = 0; i < sl->len; i++) {
        if (strcmp(sl->items[i], s) == 0) {
            return 1;
        }
    }
    return 0;
}

void dd_sl_free(dd_strlist *sl)
{
    size_t i;
    for (i = 0; i < sl->len; i++) {
        free(sl->items[i]);
    }
    free(sl->items);
    dd_sl_init(sl);
}

/* ------------------------------------------------------------------ */
/* Sizes and numbers                                                   */
/* ------------------------------------------------------------------ */

static const char *const g_bin_units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
static const char *const g_si_units[]  = { "B", "KB",  "MB",  "GB",  "TB",  "PB",  "EB"  };
#define DD_NUNITS 7

const char *dd_human(uint64_t bytes, dd_size_mode mode, char *buf, size_t bufsz)
{
    const char *const *units = (mode == DD_SIZE_SI) ? g_si_units : g_bin_units;
    const double div = (mode == DD_SIZE_SI) ? 1000.0 : 1024.0;
    double v = (double)bytes;
    int u = 0;

    while (v >= div && u < DD_NUNITS - 1) {
        v /= div;
        u++;
    }
    if (u == 0) {
        snprintf(buf, bufsz, "%" PRIu64 " B", bytes);
    } else if (v < 10.0) {
        snprintf(buf, bufsz, "%.1f %s", v, units[u]);
    } else {
        snprintf(buf, bufsz, "%.0f %s", v, units[u]);
    }
    return buf;
}

int dd_parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    double v;
    uint64_t mult = 1;

    if (!s || !*s) {
        return -1;
    }
    v = strtod(s, &end);
    if (end == s) {
        return -1;
    }
    /* optional suffix: K/M/G/T/P, then optional 'i', then optional 'B' */
    if (end && *end) {
        switch (toupper((unsigned char)*end)) {
        case 'K': mult = 1024ULL; break;
        case 'M': mult = 1024ULL * 1024; break;
        case 'G': mult = 1024ULL * 1024 * 1024; break;
        case 'T': mult = 1024ULL * 1024 * 1024 * 1024; break;
        case 'P': mult = 1024ULL * 1024 * 1024 * 1024 * 1024; break;
        case 'B': mult = 1; break;
        default: return -1;
        }
        end++;
        if (*end == 'i' || *end == 'I') {
            end++;
        }
        if (*end == 'b' || *end == 'B') {
            end++;
        }
        if (*end != '\0') {
            return -1;
        }
    }
    if (v < 0.0 || v != v) { /* negative or NaN */
        return -1;
    }
    *out = (uint64_t)(v * (double)mult);
    return 0;
}

const char *dd_comma(uint64_t v, char *buf, size_t bufsz)
{
    char tmp[32];
    size_t ti = 0, bi = 0, group = 0;

    if (v == 0) {
        snprintf(buf, bufsz, "0");
        return buf;
    }
    while (v > 0) {
        tmp[ti++] = (char)('0' + (v % 10));
        v /= 10;
        group++;
        if (group == 3 && v > 0) {
            tmp[ti++] = ',';
            group = 0;
        }
    }
    while (ti > 0 && bi + 1 < bufsz) {
        buf[bi++] = tmp[--ti];
    }
    buf[bi] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

void dd_fmt_time(int64_t epoch, char *buf, size_t bufsz)
{
    time_t t = (time_t)epoch;
    struct tm tmv;

    if (localtime_r(&t, &tmv) == NULL || strftime(buf, bufsz, "%Y-%m-%d %H:%M", &tmv) == 0) {
        snprintf(buf, bufsz, "?");
    }
}

void dd_fmt_age(int64_t seconds, char *buf, size_t bufsz)
{
    double d;
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds < 60) {
        snprintf(buf, bufsz, "%" PRId64 "s", seconds);
        return;
    }
    if (seconds < 3600) {
        snprintf(buf, bufsz, "%" PRId64 "m", seconds / 60);
        return;
    }
    if (seconds < 86400) {
        snprintf(buf, bufsz, "%" PRId64 "h", seconds / 3600);
        return;
    }
    d = (double)seconds / 86400.0;
    if (d < 30.0) {
        snprintf(buf, bufsz, "%.0fd", d);
    } else if (d < 365.0) {
        snprintf(buf, bufsz, "%.1fmo", d / 30.4);
    } else {
        snprintf(buf, bufsz, "%.1fy", d / 365.0);
    }
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

const char *dd_basename(const char *path)
{
    const char *slash;
    if (!path || !*path) {
        return path ? path : "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void dd_dirname(const char *path, dd_strbuf *out)
{
    const char *slash;
    dd_sb_reset(out);
    if (!path || !*path) {
        dd_sb_append(out, ".");
        return;
    }
    slash = strrchr(path, '/');
    if (!slash) {
        dd_sb_append(out, ".");
        return;
    }
    if (slash == path) {
        dd_sb_append(out, "/");
        return;
    }
    dd_sb_appendn(out, path, (size_t)(slash - path));
}

const char *dd_extension(const char *path, char *buf, size_t bufsz)
{
    const char *base = dd_basename(path);
    const char *dot = strrchr(base, '.');
    size_t i = 0;

    if (bufsz == 0) {
        return buf;
    }
    if (!dot || dot == base || dot[1] == '\0') {
        snprintf(buf, bufsz, "(none)");
        return buf;
    }
    for (dot = dot + 1; *dot && i + 1 < bufsz; dot++) {
        buf[i++] = (char)tolower((unsigned char)*dot);
    }
    buf[i] = '\0';
    return buf;
}

int dd_name_is_hidden(const char *name)
{
    return name && name[0] == '.' && !(name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/* ------------------------------------------------------------------ */
/* Colors                                                              */
/* ------------------------------------------------------------------ */

void dd_color_apply(int mode)
{
    if (mode == DD_COLOR_FORCE_ON) {
        dd_color_on = 1;
        return;
    }
    if (mode == DD_COLOR_FORCE_OFF) {
        dd_color_on = 0;
        return;
    }
    dd_color_on = isatty(fileno(stdout)) &&
                  getenv("NO_COLOR") == NULL &&
                  getenv("TERM") != NULL &&
                  strcmp(getenv("TERM"), "dumb") != 0;
}

const char *dd_c(const char *code)
{
    return dd_color_on ? code : "";
}

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */
/* ------------------------------------------------------------------ */

int dd_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int dd_out_ok(FILE *f)
{
    return ferror(f) == 0;
}

/* ------------------------------------------------------------------ */
/* Bars and JSON                                                       */
/* ------------------------------------------------------------------ */

void dd_bar(dd_strbuf *sb, double frac, int width)
{
    int filled, i;
    static const char *full = "\xE2\x96\x88"; /* U+2588 FULL BLOCK   */
    static const char *empty = "\xE2\x96\x91"; /* U+2591 LIGHT SHADE */

    if (frac < 0.0) {
        frac = 0.0;
    }
    if (frac > 1.0) {
        frac = 1.0;
    }
    filled = (int)(frac * (double)width + 0.5);
    for (i = 0; i < width; i++) {
        dd_sb_append(sb, i < filled ? full : empty);
    }
}

void dd_json_quote(FILE *out, const char *s)
{
    const unsigned char *p;

    DD_FPUTC('"', out);
    if (s) {
        for (p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"':  DD_FPUTS("\\\"", out); break;
            case '\\': DD_FPUTS("\\\\", out); break;
            case '\n': DD_FPUTS("\\n", out);  break;
            case '\r': DD_FPUTS("\\r", out);  break;
            case '\t': DD_FPUTS("\\t", out);  break;
            case '\b': DD_FPUTS("\\b", out);  break;
            case '\f': DD_FPUTS("\\f", out);  break;
            default:
                if (*p < 0x20) {
                    dd_fprintf(out, "\\u%04x", (unsigned)*p);
                } else {
                    DD_FPUTC((char)*p, out);
                }
                break;
            }
        }
    }
    DD_FPUTC('"', out);
}
