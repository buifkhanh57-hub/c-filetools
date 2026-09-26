/*
 * dupfind.c — size prefilter → FNV-1a hashing → byte-exact verification.
 *
 * The implementation avoids hash maps entirely: candidate sets are found
 * by sorting index arrays, which keeps the code deterministic and easy
 * to reason about even for very large trees.
 */
#include "args.h"
#include "dupfind.h"
#include "hash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Byte-exact comparison of two files                                  */
/* ------------------------------------------------------------------ */

/**
 * Compare two files chunk by chunk.
 * Returns 1 identical, 0 different, -1 on I/O error (@p err filled).
 */
static int files_identical(const char *pa, const char *pb, char *err, size_t errn)
{
    int fa = -1, fb = -1;
    char *ba = NULL, *bb = NULL;
    int rc = -1;

    fa = open(pa, O_RDONLY);
    if (fa < 0) {
        snprintf(err, errn, "'%s': %s", pa, strerror(errno));
        goto out;
    }
    fb = open(pb, O_RDONLY);
    if (fb < 0) {
        snprintf(err, errn, "'%s': %s", pb, strerror(errno));
        goto out;
    }
    ba = dd_xmalloc(DD_HASH_CHUNK);
    bb = dd_xmalloc(DD_HASH_CHUNK);

    for (;;) {
        ssize_t na = 0, nb = 0;
        for (;;) {
            ssize_t g = read(fa, ba + na, DD_HASH_CHUNK - (size_t)na);
            if (g < 0) {
                if (errno == EINTR) {
                    continue;
                }
                snprintf(err, errn, "'%s': %s", pa, strerror(errno));
                goto out;
            }
            if (g == 0) {
                break;
            }
            na += g;
            if ((size_t)na == DD_HASH_CHUNK) {
                break;
            }
        }
        for (;;) {
            ssize_t g = read(fb, bb + nb, DD_HASH_CHUNK - (size_t)nb);
            if (g < 0) {
                if (errno == EINTR) {
                    continue;
                }
                snprintf(err, errn, "'%s': %s", pb, strerror(errno));
                goto out;
            }
            if (g == 0) {
                break;
            }
            nb += g;
            if ((size_t)nb == DD_HASH_CHUNK) {
                break;
            }
        }
        if (na != nb) {
            rc = 0;
            goto out;
        }
        if (na == 0) {
            rc = 1; /* both exhausted with equal prefixes */
            goto out;
        }
        if (memcmp(ba, bb, (size_t)na) != 0) {
            rc = 0;
            goto out;
        }
    }

out:
    free(ba);
    free(bb);
    if (fa >= 0) {
        close(fa);
    }
    if (fb >= 0) {
        close(fb);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Result helpers                                                      */
/* ------------------------------------------------------------------ */

static void res_add_group(dup_result *res, uint64_t size,
                          const size_t *idx, size_t n, const file_list *fl)
{
    dup_group *g;
    size_t i;

    if (res->len == res->cap) {
        res->cap = res->cap ? res->cap * 2 : 16;
        res->groups = dd_xrealloc(res->groups, res->cap * sizeof(*res->groups));
    }
    g = &res->groups[res->len++];
    g->size = size;
    g->nfiles = n;
    g->paths = dd_xmalloc(n * sizeof(*g->paths));
    for (i = 0; i < n; i++) {
        g->paths[i] = dd_xstrdup(fl->items[idx[i]].path);
    }
    res->redundant_files += n - 1;
    res->wasted_bytes += (n - 1) * size;
}

void dups_free(dup_result *res)
{
    size_t i, j;
    for (i = 0; i < res->len; i++) {
        for (j = 0; j < res->groups[i].nfiles; j++) {
            free(res->groups[i].paths[j]);
        }
        free(res->groups[i].paths);
    }
    free(res->groups);
    memset(res, 0, sizeof(*res));
}

/* ------------------------------------------------------------------ */
/* Sorting helpers (single-threaded static keys keep qsort simple)     */
/* ------------------------------------------------------------------ */

static const file_list *g_fl;

static int idx_by_devino(const void *a, const void *b)
{
    const file_entry *fa = &g_fl->items[*(const size_t *)a];
    const file_entry *fb = &g_fl->items[*(const size_t *)b];
    if (fa->dev != fb->dev) {
        return fa->dev < fb->dev ? -1 : 1;
    }
    if (fa->ino != fb->ino) {
        return fa->ino < fb->ino ? -1 : 1;
    }
    return strcmp(fa->path, fb->path);
}

static int idx_by_size(const void *a, const void *b)
{
    const file_entry *fa = &g_fl->items[*(const size_t *)a];
    const file_entry *fb = &g_fl->items[*(const size_t *)b];
    if (fa->size != fb->size) {
        return fa->size < fb->size ? -1 : 1;
    }
    return strcmp(fa->path, fb->path);
}

/* ------------------------------------------------------------------ */
/* Pipeline                                                            */
/* ------------------------------------------------------------------ */

int dups_find(const char *root, const scan_options *so, const dup_options *o,
              dup_result *res, scan_stats *st, char *err, size_t errn)
{
    file_list fl;
    size_t *idx = NULL;
    size_t n = 0, i;
    char ioerr[512];

    memset(res, 0, sizeof(*res));
    g_fl = &fl;

    if (scan_collect_files(root, so, &fl, st, err, errn) != 0) {
        return -1;
    }

    /* stage 0: min-size filter */
    idx = dd_xmalloc((fl.len ? fl.len : 1) * sizeof(*idx));
    for (i = 0; i < fl.len; i++) {
        if (fl.items[i].size >= o->min_size) {
            idx[n++] = i;
        }
    }

    /* stage 1: hard-link de-duplication (same dev+ino) */
    qsort(idx, n, sizeof(*idx), idx_by_devino);
    {
        size_t w = 0;
        for (i = 0; i < n; i++) {
            if (w > 0 && fl.items[idx[w - 1]].dev == fl.items[idx[i]].dev &&
                fl.items[idx[w - 1]].ino == fl.items[idx[i]].ino) {
                res->hardlink_skips++;
                continue;
            }
            idx[w++] = idx[i];
        }
        n = w;
    }

    /* stage 2: group by size */
    qsort(idx, n, sizeof(*idx), idx_by_size);
    for (i = 0; i < n;) {
        size_t j = i;
        while (j < n && fl.items[idx[j]].size == fl.items[idx[i]].size) {
            j++;
        }
        if (j - i >= 2) {
            /* hash every member of this size group */
            size_t k, m = 0;
            size_t *hidx = dd_xmalloc((j - i) * sizeof(*hidx));
            uint64_t *hv = dd_xmalloc((j - i) * sizeof(*hv));

            for (k = i; k < j; k++) {
                uint64_t h;
                if (dd_hash_file(fl.items[idx[k]].path,
                                 o->quick ? DD_HASH_CHUNK : 0, &h,
                                 ioerr, sizeof(ioerr)) != 0) {
                    dd_warn("%s", ioerr);
                    st->errors++;
                    continue;
                }
                hidx[m] = idx[k];
                hv[m] = h;
                m++;
            }

            /* insertion sort by (hash, path) — small groups */
            for (k = 1; k < m; k++) {
                size_t ki = hidx[k];
                uint64_t kh = hv[k];
                size_t p = k;
                while (p > 0 && (hv[p - 1] > kh ||
                                 (hv[p - 1] == kh &&
                                  strcmp(fl.items[hidx[p - 1]].path,
                                         fl.items[ki].path) > 0))) {
                    hidx[p] = hidx[p - 1];
                    hv[p] = hv[p - 1];
                    p--;
                }
                hidx[p] = ki;
                hv[p] = kh;
            }

            /* runs of equal hash → byte-exact verification */
            for (k = 0; k < m;) {
                size_t e = k;
                while (e < m && hv[e] == hv[k]) {
                    e++;
                }
                if (e - k >= 2) {
                    size_t head = hidx[k];
                    size_t start = k;
                    size_t x = k + 1;
                    while (x < e) {
                        int same = 0;
                        if (fl.items[hidx[x]].size == fl.items[head].size) {
                            same = files_identical(fl.items[head].path,
                                                   fl.items[hidx[x]].path,
                                                   ioerr, sizeof(ioerr));
                            if (same < 0) {
                                dd_warn("%s", ioerr);
                                st->errors++;
                                same = 0;
                            }
                        }
                        if (same) {
                            x++;
                            continue;
                        }
                        if (x - start >= 2) {
                            res_add_group(res, fl.items[head].size,
                                          &hidx[start], x - start, &fl);
                        }
                        head = hidx[x];
                        start = x;
                        x++;
                    }
                    if (e - start >= 2) {
                        res_add_group(res, fl.items[head].size,
                                      &hidx[start], e - start, &fl);
                    }
                }
                k = e;
            }

            free(hidx);
            free(hv);
        }
        i = j;
    }

    /* groups ordered by wasted bytes (desc), then size, then first path */
    {
        size_t a, b;
        for (a = 0; a + 1 < res->len; a++) {
            for (b = 0; b + 1 < res->len - a; b++) {
                dup_group *g1 = &res->groups[b];
                dup_group *g2 = &res->groups[b + 1];
                uint64_t w1 = (g1->nfiles - 1) * g1->size;
                uint64_t w2 = (g2->nfiles - 1) * g2->size;
                int swap = 0;
                if (w1 != w2) {
                    swap = w1 < w2;
                } else if (g1->size != g2->size) {
                    swap = g1->size < g2->size;
                } else {
                    swap = strcmp(g1->paths[0], g2->paths[0]) > 0;
                }
                if (swap) {
                    dup_group tmp = *g1;
                    *g1 = *g2;
                    *g2 = tmp;
                }
            }
        }
    }

    res->scanned_bytes = st->total_bytes;

    free(idx);
    file_list_free(&fl);
    g_fl = NULL;
    (void)err;
    (void)errn;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

int dups_print(const dup_result *res, const char *rootpath, int summary,
               int bytes_mode, int si, FILE *out)
{
    dd_size_mode sm = si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32], hwaste[32], nbuf[32];

    if (!summary) {
        dd_fprintf(out, "%sDuplicate files in %s%s (FNV-1a/64, byte-exact verified)\n",
                   dd_c(DD_C_BOLD), rootpath, dd_c(DD_C_RESET));
    }
    if (res->hardlink_skips > 0) {
        dd_fprintf(out, "Skipped %llu hard-linked file%s (same inode)\n",
                   (unsigned long long)res->hardlink_skips,
                   res->hardlink_skips == 1 ? "" : "s");
    }
    if (res->len == 0) {
        DD_FPUTS("No duplicate files found.\n", out);
    } else if (!summary) {
        size_t i, j;
        for (i = 0; i < res->len; i++) {
            const dup_group *g = &res->groups[i];
            if (bytes_mode) {
                dd_comma(g->size, hsize, sizeof(hsize));
                dd_comma((g->nfiles - 1) * g->size, hwaste, sizeof(hwaste));
            } else {
                dd_human(g->size, sm, hsize, sizeof(hsize));
                dd_human((g->nfiles - 1) * g->size, sm, hwaste, sizeof(hwaste));
            }
            dd_fprintf(out, "%sGroup %llu%s: %llu file%s × %s — wasted %s\n",
                       dd_c(DD_C_BOLD), (unsigned long long)(i + 1), dd_c(DD_C_RESET),
                       (unsigned long long)g->nfiles, g->nfiles == 1 ? "" : "s",
                       hsize, hwaste);
            for (j = 0; j < g->nfiles; j++) {
                dd_fprintf(out, "  [%llu] %s%s%s\n",
                           (unsigned long long)(j + 1),
                           dd_c(DD_C_CYAN), g->paths[j], dd_c(DD_C_RESET));
            }
        }
    }
    dd_fprintf(out, "Summary: %s duplicate group%s · %s redundant file%s · %s wasted",
               dd_comma((uint64_t)res->len, nbuf, sizeof(nbuf)),
               res->len == 1 ? "" : "s",
               dd_comma((uint64_t)res->redundant_files, hsize, sizeof(hsize)),
               res->redundant_files == 1 ? "" : "s",
               bytes_mode ? dd_comma(res->wasted_bytes, hwaste, sizeof(hwaste))
                          : dd_human(res->wasted_bytes, sm, hwaste, sizeof(hwaste)));
    if (res->scanned_bytes > 0 && res->wasted_bytes > 0) {
        dd_fprintf(out, " (%.1f%% of scanned %s)",
                   100.0 * (double)res->wasted_bytes / (double)res->scanned_bytes,
                   dd_human(res->scanned_bytes, sm, hsize, sizeof(hsize)));
    }
    DD_FPUTC('\n', out);
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

int dups_json(const dup_result *res, const char *rootpath,
              const dup_options *o, FILE *out)
{
    size_t i, j;

    dd_fprintf(out, "{\"path\":");
    dd_json_quote(out, rootpath);
    dd_fprintf(out, ",\"min_size\":%llu,\"quick\":%s,"
                    "\"groups\":%llu,\"redundant_files\":%llu,"
                    "\"wasted_bytes\":%llu,\"hardlink_skips\":%llu,\"items\":[",
               (unsigned long long)o->min_size, o->quick ? "true" : "false",
               (unsigned long long)res->len,
               (unsigned long long)res->redundant_files,
               (unsigned long long)res->wasted_bytes,
               (unsigned long long)res->hardlink_skips);
    for (i = 0; i < res->len; i++) {
        const dup_group *g = &res->groups[i];
        if (i > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"size\":%llu,\"files\":%llu,\"wasted\":%llu,\"paths\":[",
                   (unsigned long long)g->size,
                   (unsigned long long)g->nfiles,
                   (unsigned long long)((g->nfiles - 1) * g->size));
        for (j = 0; j < g->nfiles; j++) {
            if (j > 0) {
                DD_FPUTC(',', out);
            }
            dd_json_quote(out, g->paths[j]);
        }
        DD_FPUTS("]}", out);
    }
    DD_FPUTS("]}", out);
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* dups command                                                        */
/* ------------------------------------------------------------------ */

enum { DD_DUPS_MIN_SIZE = 1, DD_DUPS_QUICK, DD_DUPS_SUMMARY, DD_DUPS_BYTES,
       DD_DUPS_FOLLOW };

static const dd_option g_dups_opts[] = {
    { "min-size", 'm', 1, DD_DUPS_MIN_SIZE, "SIZE",
      "ignore files smaller than SIZE (default: 1 byte)" },
    { "quick", 'q', 0, DD_DUPS_QUICK, NULL,
      "hash only the first 64 KiB of each file (still verified)" },
    { "summary", 's', 0, DD_DUPS_SUMMARY, NULL,
      "print only the summary line, no groups" },
    { "bytes", 'b', 0, DD_DUPS_BYTES, NULL,
      "print raw byte counts instead of human units" },
    { "follow", 'L', 0, DD_DUPS_FOLLOW, NULL,
      "follow symbolic links (default: never)" }
};

static void dups_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s dups [PATH] [options]\n\n"
        "Find duplicate files with a three-stage pipeline:\n"
        "  1. size prefilter — different sizes can never be duplicates;\n"
        "  2. FNV-1a/64 streaming hash in 64 KiB chunks;\n"
        "  3. byte-exact verification, so hash collisions are harmless.\n"
        "Hard links to the same inode are detected and reported, not\n"
        "counted as duplicates.\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_dups_opts, DD_NELTS(g_dups_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --json --si --color --no-color -h\n"
             "\nExamples:\n"
             "  diskdive dups ~/photos\n"
             "  diskdive dups . --min-size 1M --quick\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int dups_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    dup_options o;
    scan_options so;
    dup_result res;
    scan_stats st;
    dd_args a;
    const char *path;
    char err[256];
    size_t i;
    int summary = 0, follow = 0, bytes_mode = 0, rc;

    memset(&o, 0, sizeof(o));
    o.min_size = 1;

    if (dd_args_parse(argc - 1, argv + 1, g_dups_opts, DD_NELTS(g_dups_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("dups", err, g_dups_opts, DD_NELTS(g_dups_opts));
    }
    if (g->help) {
        dups_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (a.npos > 1) {
        dd_usage_error("dups", "too many arguments", g_dups_opts, DD_NELTS(g_dups_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_DUPS_MIN_SIZE:
            if (dd_parse_size(p->val, &o.min_size) != 0) {
                snprintf(err, sizeof(err), "invalid size '%s' for --min-size", p->val);
                dd_usage_error("dups", err, g_dups_opts, DD_NELTS(g_dups_opts));
            }
            break;
        case DD_DUPS_QUICK:
            o.quick = 1;
            break;
        case DD_DUPS_SUMMARY:
            summary = 1;
            break;
        case DD_DUPS_BYTES:
            bytes_mode = 1;
            break;
        case DD_DUPS_FOLLOW:
            follow = 1;
            break;
        default:
            break;
        }
    }

    scan_options_defaults(&so);
    so.follow_symlinks = follow;

    scan_stats_init(&st);
    rc = dups_find(path, &so, &o, &res, &st, err, sizeof(err));
    if (rc != 0) {
        dd_error("%s", err);
        dd_args_free(&a);
        return DD_EXIT_RUNTIME;
    }
    rc = g->json ? dups_json(&res, path, &o, stdout)
                 : dups_print(&res, path, summary, bytes_mode, g->si, stdout);
    dups_free(&res);
    dd_args_free(&a);
    if (rc != 0 || !dd_out_ok(stdout)) {
        if (!dd_out_ok(stdout)) {
            dd_error("write error");
        }
        return DD_EXIT_RUNTIME;
    }
    return DD_EXIT_OK;
}
