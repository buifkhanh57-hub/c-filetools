/*
 * args.c — implementation of the shared option parser.
 *
 * The parser is intentionally small and predictable: no getopt, no
 * globals hidden from the caller, deterministic error messages. A single
 * dd_globals instance is shared process-wide so options may appear both
 * before and after the subcommand:
 *     diskdive --json report /tmp    and    diskdive report /tmp --json
 * behave identically.
 */
#include "args.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static dd_globals g_globals = { 0, 0, DD_COLOR_AUTO, 0 };

dd_globals *dd_globals_get(void)
{
    return &g_globals;
}

void dd_globals_init(dd_globals *g)
{
    g->json = 0;
    g->si = 0;
    g->color_mode = DD_COLOR_AUTO;
    g->help = 0;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void set_err(char *err, size_t errn, const char *fmt, ...)
{
    va_list ap;
    if (!err || errn == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errn, fmt, ap);
    va_end(ap);
}

static const dd_option *find_long(const dd_option *tbl, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(tbl[i].lname, name) == 0) {
            return &tbl[i];
        }
    }
    return NULL;
}

static const dd_option *find_short(const dd_option *tbl, size_t n, char c)
{
    size_t i;
    if (c == '\0') {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (tbl[i].sname == c) {
            return &tbl[i];
        }
    }
    return NULL;
}

static dd_args *args_store(dd_args *a, int id, const char *val)
{
    if (a->npairs == a->paircap) {
        a->paircap = a->paircap ? a->paircap * 2 : 16;
        a->pairs = dd_xrealloc(a->pairs, a->paircap * sizeof(*a->pairs));
    }
    a->pairs[a->npairs].id = id;
    a->pairs[a->npairs].val = val;
    a->npairs++;
    return a;
}

static void pos_store(dd_args *a, char *tok)
{
    if (a->npos == a->poscap) {
        a->poscap = a->poscap ? a->poscap * 2 : 8;
        a->pos = dd_xrealloc(a->pos, a->poscap * sizeof(*a->pos));
    }
    a->pos[a->npos++] = tok;
}

/* Handle a global option; returns 1 if consumed, 0 otherwise. */
static int global_option(const char *lname, dd_globals *g)
{
    if (strcmp(lname, "json") == 0) {
        g->json = 1;
        return 1;
    }
    if (strcmp(lname, "si") == 0) {
        g->si = 1;
        return 1;
    }
    if (strcmp(lname, "color") == 0) {
        g->color_mode = DD_COLOR_FORCE_ON;
        return 1;
    }
    if (strcmp(lname, "no-color") == 0) {
        g->color_mode = DD_COLOR_FORCE_OFF;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parser core                                                         */
/* ------------------------------------------------------------------ */

int dd_args_parse(int argc, char **argv, const dd_option *tbl, size_t ntbl,
                  dd_globals *g, dd_args *out, char *err, size_t errn)
{
    int i;
    int only_pos = 0;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < argc; i++) {
        char *tok = argv[i];

        if (only_pos) {
            pos_store(out, tok);
            continue;
        }
        if (tok[0] != '-' || tok[1] == '\0') {
            pos_store(out, tok); /* plain word or "-" (stdin convention) */
            continue;
        }
        if (strcmp(tok, "--") == 0) {
            only_pos = 1;
            continue;
        }

        if (tok[1] == '-') { /* long option */
            char name[128];
            const char *eq = strchr(tok + 2, '=');
            const char *lname = tok + 2;
            const char *inline_val = NULL;
            const dd_option *opt;

            if (eq) {
                size_t nlen = (size_t)(eq - lname);
                if (nlen >= sizeof(name)) {
                    set_err(err, errn, "option name too long: '%s'", tok);
                    return -1;
                }
                memcpy(name, lname, nlen);
                name[nlen] = '\0';
                lname = name;
                inline_val = eq + 1;
            }
            if (strcmp(lname, "help") == 0) {
                g->help = 1;
                args_store(out, DD_ID_HELP, NULL);
                continue;
            }
            if (global_option(lname, g)) {
                continue;
            }
            opt = find_long(tbl, ntbl, lname);
            if (!opt) {
                set_err(err, errn, "unknown option '--%s'", lname);
                return -1;
            }
            if (opt->has_arg) {
                const char *val;
                if (inline_val) {
                    val = inline_val;
                } else if (i + 1 < argc) {
                    val = argv[++i];
                } else {
                    set_err(err, errn, "option '--%s' requires an argument", lname);
                    return -1;
                }
                args_store(out, opt->id, val);
            } else {
                if (inline_val) {
                    set_err(err, errn, "option '--%s' does not take a value", lname);
                    return -1;
                }
                args_store(out, opt->id, NULL);
            }
            continue;
        }

        /* short option cluster: -abc, -n5, -n 5 */
        {
            size_t j;
            for (j = 1; tok[j] != '\0'; j++) {
                const dd_option *opt = find_short(tbl, ntbl, tok[j]);
                if (tok[j] == 'h' && !opt) {
                    g->help = 1;
                    args_store(out, DD_ID_HELP, NULL);
                    continue;
                }
                if (!opt) {
                    set_err(err, errn, "unknown option '-%c'", tok[j]);
                    return -1;
                }
                if (opt->has_arg) {
                    const char *val;
                    if (tok[j + 1] != '\0') {
                        val = tok + j + 1; /* -n5 */
                    } else if (i + 1 < argc) {
                        val = argv[++i];   /* -n 5 */
                    } else {
                        set_err(err, errn, "option '-%c' requires an argument", tok[j]);
                        return -1;
                    }
                    args_store(out, opt->id, val);
                    break; /* value consumed the rest of the cluster */
                }
                args_store(out, opt->id, NULL);
            }
        }
    }
    /* every command prints only after parsing, so resolving the color
     * mode here (--color/--no-color/auto, incl. NO_COLOR and tty)
     * covers all subcommands without each one having to remember. */
    dd_color_apply(g->color_mode);
    return 0;
}

void dd_args_free(dd_args *a)
{
    if (!a) {
        return;
    }
    free(a->pairs);
    free(a->pos);
    a->pairs = NULL;
    a->pos = NULL;
    a->npairs = 0;
    a->npos = 0;
    a->paircap = 0;
    a->poscap = 0;
}

int dd_args_has(const dd_args *a, int id)
{
    size_t i;
    for (i = 0; i < a->npairs; i++) {
        if (a->pairs[i].id == id) {
            return 1;
        }
    }
    return 0;
}

const char *dd_args_get(const dd_args *a, int id)
{
    size_t i;
    const char *val = NULL;
    for (i = 0; i < a->npairs; i++) {
        if (a->pairs[i].id == id) {
            val = a->pairs[i].val; /* last occurrence wins */
        }
    }
    return val;
}

size_t dd_args_count(const dd_args *a, int id)
{
    size_t i, n = 0;
    for (i = 0; i < a->npairs; i++) {
        if (a->pairs[i].id == id) {
            n++;
        }
    }
    return n;
}

const char *dd_args_get_nth(const dd_args *a, int id, size_t nth)
{
    size_t i, seen = 0;
    for (i = 0; i < a->npairs; i++) {
        if (a->pairs[i].id == id) {
            if (seen == nth) {
                return a->pairs[i].val;
            }
            seen++;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Help rendering                                                      */
/* ------------------------------------------------------------------ */

void dd_print_options(FILE *out, const dd_option *tbl, size_t n)
{
    size_t i, width = 0;
    char left[96];

    for (i = 0; i < n; i++) {
        size_t w;
        if (tbl[i].sname) {
            snprintf(left, sizeof(left), "-%c, --%s%s", tbl[i].sname, tbl[i].lname,
                     tbl[i].has_arg ? " " : "");
            if (tbl[i].has_arg && tbl[i].arghint) {
                size_t cur = strlen(left);
                snprintf(left + cur, sizeof(left) - cur, "%s", tbl[i].arghint);
            }
        } else {
            snprintf(left, sizeof(left), "    --%s%s", tbl[i].lname,
                     tbl[i].has_arg ? " " : "");
            if (tbl[i].has_arg && tbl[i].arghint) {
                size_t cur = strlen(left);
                snprintf(left + cur, sizeof(left) - cur, "%s", tbl[i].arghint);
            }
        }
        w = strlen(left);
        if (w > width) {
            width = w;
        }
    }
    if (width > 30) {
        width = 30; /* keep help readable when names get long */
    }
    for (i = 0; i < n; i++) {
        if (tbl[i].sname) {
            snprintf(left, sizeof(left), "-%c, --%s%s", tbl[i].sname, tbl[i].lname,
                     tbl[i].has_arg ? " " : "");
        } else {
            snprintf(left, sizeof(left), "    --%s%s", tbl[i].lname,
                     tbl[i].has_arg ? " " : "");
        }
        if (tbl[i].has_arg && tbl[i].arghint) {
            size_t cur = strlen(left);
            snprintf(left + cur, sizeof(left) - cur, "%s", tbl[i].arghint);
        }
        dd_fprintf(out, "  %-*s  %s\n", (int)width, left, tbl[i].help);
    }
}

void dd_usage_error(const char *cmd, const char *err,
                    const dd_option *tbl, size_t n)
{
    dd_error("%s", err ? err : "invalid usage");
    if (cmd) {
        dd_fprintf(stderr, "Try '%s %s --help' for more information.\n", dd_progname, cmd);
    } else {
        dd_fprintf(stderr, "Try '%s --help' for more information.\n", dd_progname);
    }
    if (tbl && n > 0) {
        DD_FPUTS("Options:\n", stderr);
        dd_print_options(stderr, tbl, n);
    }
    exit(DD_EXIT_USAGE);
}

int dd_parse_count(const char *s, size_t *out, char *err, size_t errn)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s) {
        set_err(err, errn, "missing numeric value");
        return -1;
    }
    v = strtoull(s, &end, 10);
    if (end == s || (end && *end != '\0')) {
        set_err(err, errn, "invalid number '%s'", s);
        return -1;
    }
    if (v > 1000000000ULL) {
        set_err(err, errn, "number '%s' is out of range", s);
        return -1;
    }
    *out = (size_t)v;
    return 0;
}
