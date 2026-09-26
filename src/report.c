/*
 * report.c — composes the du/top/types/ages/dups/clean cores into a
 * single combined report (aligned text or one JSON document).
 */
#include "args.h"
#include "report.h"

#include <stdlib.h>
#include <string.h>

#include "ages.h"
#include "clean.h"
#include "dupfind.h"
#include "du.h"
#include "top.h"
#include "types.h"

/* ------------------------------------------------------------------ */
/* Shared section data (built once, printed by either backend)         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *rootpath;
    const du_node *tree;
    const scan_stats *st;
    const top_heap *toph;
    const type_map *tm;
    const age_report *ar;
    const dup_result *dr;
    const clean_list *cl;
    dd_size_mode sm;
} report_data;

/* ------------------------------------------------------------------ */
/* JSON backend                                                        */
/* ------------------------------------------------------------------ */

static void json_report(const report_data *d, FILE *out)
{
    char hsize[32];
    size_t i;
    int k;

    dd_human(d->tree->total_bytes, d->sm, hsize, sizeof(hsize));
    DD_FPUTS("{\"overview\":{\"path\":", out);
    dd_json_quote(out, d->rootpath);
    dd_fprintf(out, ",\"bytes\":%llu,\"human\":\"%s\",\"apparent_bytes\":%llu,"
                    "\"files\":%llu,\"dirs\":%llu,\"symlinks\":%llu,\"errors\":%llu}",
               (unsigned long long)d->tree->total_blocks, hsize,
               (unsigned long long)d->tree->total_bytes,
               (unsigned long long)d->tree->file_count,
               (unsigned long long)d->tree->dir_count,
               (unsigned long long)d->st->symlinks,
               (unsigned long long)d->st->errors);

    /* largest directories */
    dd_fprintf(out, ",\"top_dirs\":[");
    {
        const du_node **rows = NULL;
        size_t nrows = 0;
        du_sort_children(d->tree, DD_DUSORT_SIZE_DESC, 0, &rows, &nrows);
        for (i = 0; i < nrows && i < 5; i++) {
            const du_node *n = rows[i];
            dd_strbuf pb;
            dd_sb_init(&pb);
            du_node_path(n, d->rootpath, &pb);
            dd_human(n->total_blocks, d->sm, hsize, sizeof(hsize));
            if (i > 0) {
                DD_FPUTC(',', out);
            }
            dd_fprintf(out, "{\"path\":");
            dd_json_quote(out, pb.data);
            dd_fprintf(out, ",\"bytes\":%llu,\"human\":\"%s\",\"pct\":%.2f}",
                       (unsigned long long)n->total_blocks, hsize,
                       d->tree->total_blocks > 0
                           ? 100.0 * (double)n->total_blocks / (double)d->tree->total_blocks
                           : 0.0);
            dd_sb_free(&pb);
        }
        free((void *)rows);
    }
    DD_FPUTS("],", out);

    /* largest files */
    dd_fprintf(out, "\"top_files\":[");
    for (i = 0; i < d->toph->len; i++) {
        dd_human(d->toph->e[i].size, d->sm, hsize, sizeof(hsize));
        if (i > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"path\":");
        dd_json_quote(out, d->toph->e[i].path);
        dd_fprintf(out, ",\"bytes\":%llu,\"human\":\"%s\"}",
                   (unsigned long long)d->toph->e[i].size, hsize);
    }
    DD_FPUTS("],", out);

    /* extensions */
    dd_fprintf(out, "\"types\":[");
    for (i = 0; i < d->tm->len && i < 5; i++) {
        dd_human(d->tm->items[i].bytes, d->sm, hsize, sizeof(hsize));
        if (i > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"ext\":");
        dd_json_quote(out, d->tm->items[i].ext);
        dd_fprintf(out, ",\"files\":%llu,\"bytes\":%llu,\"human\":\"%s\"}",
                   (unsigned long long)d->tm->items[i].count,
                   (unsigned long long)d->tm->items[i].bytes, hsize);
    }
    DD_FPUTS("],", out);

    /* ages */
    dd_fprintf(out, "\"ages\":{\"buckets\":[");
    for (k = 0; k < DD_AGE_BUCKETS; k++) {
        if (k > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"name\":\"%s\",\"files\":%llu,\"bytes\":%llu}",
                   age_bucket_name(k),
                   (unsigned long long)d->ar->b[k].files,
                   (unsigned long long)d->ar->b[k].bytes);
    }
    DD_FPUTS("]}", out);

    /* duplicates */
    dd_fprintf(out, ",\"duplicates\":{\"groups\":%llu,\"redundant_files\":%llu,"
                    "\"wasted_bytes\":%llu}",
               (unsigned long long)d->dr->len,
               (unsigned long long)d->dr->redundant_files,
               (unsigned long long)d->dr->wasted_bytes);

    /* clean candidates */
    dd_fprintf(out, ",\"clean\":{\"items\":%llu,\"bytes\":%llu,\"list\":[",
               (unsigned long long)d->cl->len,
               (unsigned long long)d->cl->total_bytes);
    for (i = 0; i < d->cl->len; i++) {
        if (i > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"path\":");
        dd_json_quote(out, d->cl->items[i].path);
        dd_fprintf(out, ",\"is_dir\":%s,\"bytes\":%llu,\"reason\":",
                   d->cl->items[i].is_dir ? "true" : "false",
                   (unsigned long long)d->cl->items[i].bytes);
        dd_json_quote(out, d->cl->items[i].reason);
        DD_FPUTC('}', out);
    }
    DD_FPUTS("]}", out);

    DD_FPUTS("}", out);
}

/* ------------------------------------------------------------------ */
/* Text backend                                                        */
/* ------------------------------------------------------------------ */

static void section_header(FILE *out, const char *title)
{
    dd_fprintf(out, "\n%s== %s ==%s\n", dd_c(DD_C_BOLD), title, dd_c(DD_C_RESET));
}

static void text_report(const report_data *d, FILE *out)
{
    char hsize[32], hsize2[32], nbuf[32];
    size_t i;
    int k;

    dd_fprintf(out, "%sdiskdive report%s — %s\n",
               dd_c(DD_C_BOLD), dd_c(DD_C_RESET), d->rootpath);

    section_header(out, "Overview");
    dd_human(d->tree->total_blocks, d->sm, hsize, sizeof(hsize));
    dd_human(d->tree->total_bytes, d->sm, hsize2, sizeof(hsize2));
    dd_fprintf(out, "  Size:      %s on disk (%s apparent)\n", hsize, hsize2);
    dd_fprintf(out, "  Entries:   %s files, %s directories, %s symlinks\n",
               dd_comma(d->tree->file_count, nbuf, sizeof(nbuf)),
               dd_comma(d->tree->dir_count, hsize2, sizeof(hsize2)),
               dd_comma(d->st->symlinks, hsize, sizeof(hsize)));
    if (d->st->errors > 0) {
        dd_fprintf(out, "  %s%s entries could not be read%s\n",
                   dd_c(DD_C_RED), dd_comma(d->st->errors, nbuf, sizeof(nbuf)),
                   dd_c(DD_C_RESET));
    }

    section_header(out, "Largest directories");
    {
        const du_node **rows = NULL;
        size_t nrows = 0;
        du_sort_children(d->tree, DD_DUSORT_SIZE_DESC, 0, &rows, &nrows);
        if (nrows == 0) {
            DD_FPUTS("  (none)\n", out);
        }
        for (i = 0; i < nrows && i < 5; i++) {
            const du_node *n = rows[i];
            dd_strbuf pb;
            dd_strbuf bar;
            dd_sb_init(&pb);
            du_node_path(n, d->rootpath, &pb);
            dd_sb_init(&bar);
            dd_bar(&bar, d->tree->total_blocks > 0
                             ? (double)n->total_blocks / (double)d->tree->total_blocks
                             : 0.0,
                   10);
            dd_human(n->total_blocks, d->sm, hsize, sizeof(hsize));
            dd_fprintf(out, "  %llu. %12s  %5.1f%%  %s%s%s  %s%s%s\n",
                       (unsigned long long)(i + 1), hsize,
                       d->tree->total_blocks > 0
                           ? 100.0 * (double)n->total_blocks / (double)d->tree->total_blocks
                           : 0.0,
                       dd_c(DD_C_DIM), bar.data, dd_c(DD_C_RESET),
                       dd_c(DD_C_CYAN), pb.data, dd_c(DD_C_RESET));
            dd_sb_free(&pb);
            dd_sb_free(&bar);
        }
        free((void *)rows);
    }

    section_header(out, "Largest files");
    if (d->toph->len == 0) {
        DD_FPUTS("  (none)\n", out);
    }
    for (i = 0; i < d->toph->len; i++) {
        dd_human(d->toph->e[i].size, d->sm, hsize, sizeof(hsize));
        dd_fprintf(out, "  %llu. %12s  %s%s%s\n",
                   (unsigned long long)(i + 1), hsize,
                   dd_c(DD_C_CYAN), d->toph->e[i].path, dd_c(DD_C_RESET));
    }

    section_header(out, "Extensions");
    if (d->tm->len == 0) {
        DD_FPUTS("  (none)\n", out);
    }
    for (i = 0; i < d->tm->len && i < 5; i++) {
        dd_human(d->tm->items[i].bytes, d->sm, hsize, sizeof(hsize));
        dd_fprintf(out, "  %-10s %6s file%s %12s\n",
                   d->tm->items[i].ext,
                   dd_comma((uint64_t)d->tm->items[i].count, nbuf, sizeof(nbuf)),
                   d->tm->items[i].count == 1 ? "" : "s",
                   hsize);
    }

    section_header(out, "Age");
    for (k = 0; k < DD_AGE_BUCKETS; k++) {
        dd_human(d->ar->b[k].bytes, d->sm, hsize, sizeof(hsize));
        dd_fprintf(out, "  %-12s %6s file%s %12s\n",
                   age_bucket_name(k),
                   dd_comma(d->ar->b[k].files, nbuf, sizeof(nbuf)),
                   d->ar->b[k].files == 1 ? "" : "s",
                   hsize);
    }

    section_header(out, "Duplicates");
    dd_human(d->dr->wasted_bytes, d->sm, hsize, sizeof(hsize));
    dd_fprintf(out, "  %llu group%s · %s redundant file%s · %s wasted (quick scan)\n",
               (unsigned long long)d->dr->len, d->dr->len == 1 ? "" : "s",
               dd_comma((uint64_t)d->dr->redundant_files, nbuf, sizeof(nbuf)),
               d->dr->redundant_files == 1 ? "" : "s",
               hsize);

    section_header(out, "Clean candidates");
    dd_human(d->cl->total_bytes, d->sm, hsize, sizeof(hsize));
    dd_fprintf(out, "  %llu item%s · %s reclaimable (dry run; see 'diskdive clean')\n",
               (unsigned long long)d->cl->len, d->cl->len == 1 ? "" : "s", hsize);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int report_run(const char *root, const scan_options *so,
               const report_options *o, int json, FILE *out)
{
    du_node *tree = NULL;
    scan_stats st;
    top_heap toph;
    type_map tm;
    age_report ar;
    dup_result dr;
    clean_list cl;
    du_options dopt;
    top_options topt;
    dup_options dopts;
    clean_options copts;
    char err[512];
    int rc = DD_EXIT_OK;

    scan_stats_init(&st);
    memset(&dopt, 0, sizeof(dopt));

    if (du_analyze(root, &dopt, so, &tree, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        return DD_EXIT_RUNTIME;
    }

    memset(&topt, 0, sizeof(topt));
    topt.n = 5;
    if (top_collect(root, so, &topt, &toph, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        du_free(tree);
        return DD_EXIT_RUNTIME;
    }
    top_heap_sort_desc(&toph);

    if (types_collect(root, so, &tm, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        top_heap_free(&toph);
        du_free(tree);
        return DD_EXIT_RUNTIME;
    }
    types_sort(&tm, DD_TYPESORT_BYTES_DESC);

    if (ages_collect(root, so, &ar, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        types_free(&tm);
        top_heap_free(&toph);
        du_free(tree);
        return DD_EXIT_RUNTIME;
    }

    memset(&dopts, 0, sizeof(dopts));
    dopts.min_size = 1;
    dopts.quick = o->quick_dups;
    if (dups_find(root, so, &dopts, &dr, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        ages_free(&ar);
        types_free(&tm);
        top_heap_free(&toph);
        du_free(tree);
        return DD_EXIT_RUNTIME;
    }

    clean_options_defaults(&copts);
    if (clean_collect(root, &copts, &cl, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        clean_options_free(&copts);
        dups_free(&dr);
        ages_free(&ar);
        types_free(&tm);
        top_heap_free(&toph);
        du_free(tree);
        return DD_EXIT_RUNTIME;
    }
    clean_options_free(&copts);

    {
        report_data d;
        d.rootpath = root;
        d.tree = tree;
        d.st = &st;
        d.toph = &toph;
        d.tm = &tm;
        d.ar = &ar;
        d.dr = &dr;
        d.cl = &cl;
        d.sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
        if (json) {
            json_report(&d, out);
        } else {
            text_report(&d, out);
        }
        if (!dd_out_ok(out)) {
            rc = DD_EXIT_RUNTIME;
        }
    }

    clean_list_free(&cl);
    dups_free(&dr);
    ages_free(&ar);
    types_free(&tm);
    top_heap_free(&toph);
    du_free(tree);
    return rc;
}

/* ------------------------------------------------------------------ */
/* report command                                                      */
/* ------------------------------------------------------------------ */

enum { DD_REPORT_FULL = 1 };

static const dd_option g_report_opts[] = {
    { "full", 'f', 0, DD_REPORT_FULL, NULL,
      "hash whole files for the duplicates section (slower, exact)" }
};

static void report_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s report [PATH] [options]\n\n"
        "One compact overview of a directory tree: totals, the five\n"
        "largest directories and files, extension breakdown, age\n"
        "distribution, duplicate groups (quick scan) and clean\n"
        "candidates. Pass --json for a single machine-readable document.\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_report_opts, DD_NELTS(g_report_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --json --si --color --no-color -h\n"
             "\nExamples:\n"
             "  diskdive report ~/projects\n"
             "  diskdive report . --json > report.json\n"
             "  diskdive report . --full\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int report_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    report_options o;
    scan_options so;
    dd_args a;
    const char *path;
    char err[256];
    size_t i;
    int rc;

    memset(&o, 0, sizeof(o));
    o.quick_dups = 1;

    if (dd_args_parse(argc - 1, argv + 1, g_report_opts, DD_NELTS(g_report_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("report", err, g_report_opts, DD_NELTS(g_report_opts));
    }
    if (g->help) {
        report_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (a.npos > 1) {
        dd_usage_error("report", "too many arguments", g_report_opts,
                       DD_NELTS(g_report_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_REPORT_FULL:
            o.quick_dups = 0;
            break;
        default:
            break; /* global options already applied by the parser */
        }
    }
    o.si = g->si;

    scan_options_defaults(&so);
    rc = report_run(path, &so, &o, g->json, stdout);
    dd_args_free(&a);
    if (!dd_out_ok(stdout)) {
        dd_error("write error");
        return DD_EXIT_RUNTIME;
    }
    return rc;
}
