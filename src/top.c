/*
 * top.c — bounded min-heap of the largest files plus the table printer.
 */
#include "args.h"
#include "top.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Min-heap                                                            */
/* ------------------------------------------------------------------ */

static void sift_up(top_entry *e, size_t idx)
{
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (e[parent].size <= e[idx].size) {
            break;
        }
        top_entry tmp = e[parent];
        e[parent] = e[idx];
        e[idx] = tmp;
        idx = parent;
    }
}

static void sift_down(top_entry *e, size_t len, size_t idx)
{
    for (;;) {
        size_t left = idx * 2 + 1;
        size_t right = left + 1;
        size_t smallest = idx;
        if (left < len && e[left].size < e[smallest].size) {
            smallest = left;
        }
        if (right < len && e[right].size < e[smallest].size) {
            smallest = right;
        }
        if (smallest == idx) {
            return;
        }
        {
            top_entry tmp = e[idx];
            e[idx] = e[smallest];
            e[smallest] = tmp;
        }
        idx = smallest;
    }
}

void top_heap_init(top_heap *h, size_t n)
{
    memset(h, 0, sizeof(*h));
    h->n = n;
    h->cap = n ? n : 1;
    h->e = dd_xcalloc(h->cap, sizeof(*h->e));
}

int top_heap_offer(top_heap *h, const char *path, uint64_t size, int64_t mtime)
{
    if (h->n == 0) {
        return 0;
    }
    if (h->len < h->n) {
        if (h->len == h->cap) {
            h->cap *= 2;
            h->e = dd_xrealloc(h->e, h->cap * sizeof(*h->e));
        }
        h->e[h->len].path = dd_xstrdup(path);
        h->e[h->len].size = size;
        h->e[h->len].mtime = mtime;
        sift_up(h->e, h->len);
        h->len++;
        return 1;
    }
    if (size <= h->e[0].size) {
        return 0;
    }
    free(h->e[0].path);
    h->e[0].path = dd_xstrdup(path);
    h->e[0].size = size;
    h->e[0].mtime = mtime;
    sift_down(h->e, h->len, 0);
    return 1;
}

static int top_desc_cmp(const void *a, const void *b)
{
    const top_entry *pa = a;
    const top_entry *pb = b;
    if (pa->size != pb->size) {
        return pa->size > pb->size ? -1 : 1;
    }
    return strcmp(pa->path, pb->path);
}

void top_heap_sort_desc(top_heap *h)
{
    qsort(h->e, h->len, sizeof(*h->e), top_desc_cmp);
}

void top_heap_free(top_heap *h)
{
    size_t i;
    for (i = 0; i < h->len; i++) {
        free(h->e[i].path);
    }
    free(h->e);
    h->e = NULL;
    h->len = 0;
    h->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    top_heap *h;
    uint64_t min_size;
} top_ctx;

static void top_on_file(void *ud, const char *path, const struct stat *st, int depth)
{
    top_ctx *c = ud;
    (void)depth;
    if ((uint64_t)st->st_size < c->min_size) {
        return;
    }
    top_heap_offer(c->h, path, (uint64_t)st->st_size, (int64_t)st->st_mtime);
}

int top_collect(const char *root, const scan_options *so, const top_options *o,
                top_heap *h, scan_stats *st, char *err, size_t errn)
{
    scan_visitor v;
    top_ctx c;

    top_heap_init(h, o->n);
    c.h = h;
    c.min_size = o->min_size;

    memset(&v, 0, sizeof(v));
    v.ud = &c;
    v.on_file = top_on_file;
    return scan_walk(root, so, &v, st, err, errn);
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

static int top_run_out(const char *rootpath, const top_options *o, const top_heap *h,
                       const scan_stats *st, FILE *out)
{
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32], nfiles[32], htot[32];
    size_t i;

    dd_fprintf(out, "%sLargest files in %s%s (top %llu, apparent sizes)\n",
               dd_c(DD_C_BOLD), rootpath, dd_c(DD_C_RESET),
               (unsigned long long)h->n);
    if (h->len == 0) {
        DD_FPUTS("No files matched.\n", out);
        return 0;
    }
    dd_fprintf(out, "  %3s  %12s  %8s  %-16s  %s\n",
               "#", "SIZE", "% TOTAL", "LAST MODIFIED", "PATH");
    for (i = 0; i < h->len; i++) {
        const top_entry *e = &h->e[i];
        char htime[32];
        dd_fmt_time(e->mtime, htime, sizeof(htime));
        if (o->bytes_mode) {
            dd_comma(e->size, hsize, sizeof(hsize));
        } else {
            dd_human(e->size, sm, hsize, sizeof(hsize));
        }
        dd_fprintf(out, "%s%4llu%s  %12s  %7.1f%%  %s  %s%s%s\n",
                   dd_c(DD_C_BOLD), (unsigned long long)(i + 1), dd_c(DD_C_RESET),
                   hsize,
                   st->total_bytes > 0
                       ? (double)e->size * 100.0 / (double)st->total_bytes
                       : 0.0,
                   htime,
                   dd_c(DD_C_CYAN), e->path, dd_c(DD_C_RESET));
    }
    dd_fprintf(out, "%s files scanned, %s apparent\n",
               dd_comma(st->files, nfiles, sizeof(nfiles)),
               dd_human(st->total_bytes, sm, htot, sizeof(htot)));
    return dd_out_ok(out) ? 0 : -1;
}

int top_run(const char *root, const scan_options *so, const top_options *o, FILE *out)
{
    top_heap h;
    scan_stats st;
    char err[512];
    int rc;

    scan_stats_init(&st);
    if (top_collect(root, so, o, &h, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        return DD_EXIT_RUNTIME;
    }
    top_heap_sort_desc(&h);
    rc = top_run_out(root, o, &h, &st, out);
    top_heap_free(&h);
    return rc == 0 ? DD_EXIT_OK : DD_EXIT_RUNTIME;
}

int top_json(const top_heap *h, const char *rootpath, const scan_stats *st,
             const top_options *o, FILE *out)
{
    size_t i;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;

    dd_fprintf(out, "{\"path\":");
    dd_json_quote(out, rootpath);
    dd_fprintf(out, ",\"requested\":%llu,\"count\":%llu,\"total_files\":%llu,"
                    "\"total_bytes\":%llu,\"files\":[",
               (unsigned long long)h->n,
               (unsigned long long)h->len,
               (unsigned long long)st->files,
               (unsigned long long)st->total_bytes);
    for (i = 0; i < h->len; i++) {
        const top_entry *e = &h->e[i];
        char hsize[32], htime[32];
        dd_human(e->size, sm, hsize, sizeof(hsize));
        dd_fmt_time(e->mtime, htime, sizeof(htime));
        if (i > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"rank\":%llu,\"path\":", (unsigned long long)(i + 1));
        dd_json_quote(out, e->path);
        dd_fprintf(out, ",\"bytes\":%llu,\"human\":\"%s\",\"mtime\":%lld,\"mtime_iso\":\"%s\"}",
                   (unsigned long long)e->size, hsize,
                   (long long)e->mtime, htime);
    }
    DD_FPUTS("]}", out);
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* top command                                                         */
/* ------------------------------------------------------------------ */

enum { DD_TOP_LIMIT = 1, DD_TOP_MIN_SIZE, DD_TOP_BYTES, DD_TOP_FOLLOW };

static const dd_option g_top_opts[] = {
    { "limit", 'n', 1, DD_TOP_LIMIT, "N",
      "how many files to report (default 20)" },
    { "min-size", 'm', 1, DD_TOP_MIN_SIZE, "SIZE",
      "skip files smaller than SIZE (e.g. 10M, 1G)" },
    { "bytes", 'b', 0, DD_TOP_BYTES, NULL,
      "print raw byte counts instead of human units" },
    { "follow", 'L', 0, DD_TOP_FOLLOW, NULL,
      "follow symbolic links (default: never)" }
};

static void top_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s top [PATH] [options]\n\n"
        "Report the N largest regular files, ranked by apparent size.\n"
        "Collection uses a bounded streaming min-heap, so memory stays\n"
        "constant regardless of how many files are scanned.\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_top_opts, DD_NELTS(g_top_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --json --si --color --no-color -h\n"
             "\nExamples:\n"
             "  diskdive top ~ -n 25\n"
             "  diskdive top /var --min-size 100M --bytes\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int top_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    top_options o;
    scan_options so;
    dd_args a;
    const char *path;
    char err[256];
    size_t i;
    int rc;
    int follow = 0;

    memset(&o, 0, sizeof(o));
    o.n = 20;

    if (dd_args_parse(argc - 1, argv + 1, g_top_opts, DD_NELTS(g_top_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("top", err, g_top_opts, DD_NELTS(g_top_opts));
    }
    if (g->help) {
        top_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (a.npos > 1) {
        dd_usage_error("top", "too many arguments", g_top_opts, DD_NELTS(g_top_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_TOP_LIMIT: {
            size_t v;
            if (dd_parse_count(p->val, &v, err, sizeof(err)) != 0 || v == 0) {
                if (err[0] == '\0') {
                    snprintf(err, sizeof(err), "--limit expects N >= 1");
                }
                dd_usage_error("top", err, g_top_opts, DD_NELTS(g_top_opts));
            }
            o.n = v;
            break;
        }
        case DD_TOP_MIN_SIZE:
            if (dd_parse_size(p->val, &o.min_size) != 0) {
                snprintf(err, sizeof(err), "invalid size '%s' for --min-size", p->val);
                dd_usage_error("top", err, g_top_opts, DD_NELTS(g_top_opts));
            }
            break;
        case DD_TOP_BYTES:
            o.bytes_mode = 1;
            break;
        case DD_TOP_FOLLOW:
            follow = 1;
            break;
        default:
            break;
        }
    }

    o.si = g->si;
    scan_options_defaults(&so);
    so.follow_symlinks = follow;

    if (g->json) {
        top_heap h;
        scan_stats st;
        char ferr[512];

        scan_stats_init(&st);
        if (top_collect(path, &so, &o, &h, &st, ferr, sizeof(ferr)) != 0) {
            dd_error("%s", ferr);
            dd_args_free(&a);
            return DD_EXIT_RUNTIME;
        }
        top_heap_sort_desc(&h);
        rc = top_json(&h, path, &st, &o, stdout);
        top_heap_free(&h);
    } else {
        rc = top_run(path, &so, &o, stdout);
    }
    dd_args_free(&a);
    if (!dd_out_ok(stdout)) {
        dd_error("write error");
        return DD_EXIT_RUNTIME;
    }
    return rc;
}
