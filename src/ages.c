/*
 * ages.c — mtime bucketing implementation.
 */
#include "args.h"
#include "ages.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *age_bucket_name(int bucket)
{
    static const char *const names[DD_AGE_BUCKETS] = {
        "<1 day", "1-7 days", "7-30 days", "30-365 days", ">1 year"
    };
    if (bucket < 0 || bucket >= DD_AGE_BUCKETS) {
        return "?";
    }
    return names[bucket];
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    age_report *ar;
} ages_ctx;

static int bucket_for(int64_t age)
{
    if (age < DD_AGE_1D) {
        return 0;
    }
    if (age < DD_AGE_7D) {
        return 1;
    }
    if (age < DD_AGE_30D) {
        return 2;
    }
    if (age < DD_AGE_1Y) {
        return 3;
    }
    return 4;
}

static void ages_on_file(void *ud, const char *path, const struct stat *st, int depth)
{
    ages_ctx *c = ud;
    int64_t age;
    (void)depth;

    age = c->ar->now - (int64_t)st->st_mtime;
    if (age < 0) {
        age = 0; /* clock skew: treat future mtimes as brand new */
    }
    c->ar->b[bucket_for(age)].files++;
    c->ar->b[bucket_for(age)].bytes += (uint64_t)st->st_size;

    if (!c->ar->newest_path || (int64_t)st->st_mtime > c->ar->newest_mtime) {
        free(c->ar->newest_path);
        c->ar->newest_path = dd_xstrdup(path);
        c->ar->newest_mtime = (int64_t)st->st_mtime;
    }
    if (!c->ar->oldest_path || (int64_t)st->st_mtime < c->ar->oldest_mtime) {
        free(c->ar->oldest_path);
        c->ar->oldest_path = dd_xstrdup(path);
        c->ar->oldest_mtime = (int64_t)st->st_mtime;
    }
}

int ages_collect(const char *root, const scan_options *so, age_report *ar,
                 scan_stats *st, char *err, size_t errn)
{
    scan_visitor v;
    ages_ctx c;

    memset(ar, 0, sizeof(*ar));
    ar->now = (int64_t)time(NULL);
    c.ar = ar;

    memset(&v, 0, sizeof(v));
    v.ud = &c;
    v.on_file = ages_on_file;
    return scan_walk(root, so, &v, st, err, errn);
}

void ages_free(age_report *ar)
{
    free(ar->newest_path);
    free(ar->oldest_path);
    ar->newest_path = NULL;
    ar->oldest_path = NULL;
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

int ages_print(const age_report *ar, const char *rootpath,
               const ages_options *o, FILE *out)
{
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32], nbuf[32], tbuf[32], abuf[32];
    uint64_t total_files = 0;
    int i;

    for (i = 0; i < DD_AGE_BUCKETS; i++) {
        total_files += ar->b[i].files;
    }

    dd_fprintf(out, "%sFile age distribution in %s%s (by mtime)\n",
               dd_c(DD_C_BOLD), rootpath, dd_c(DD_C_RESET));
    if (total_files == 0) {
        DD_FPUTS("No files found.\n", out);
        return 0;
    }
    dd_fprintf(out, "  %-12s  %8s  %12s  %7s  %s\n",
               "AGE", "FILES", "SIZE", "%", "BAR");
    for (i = 0; i < DD_AGE_BUCKETS; i++) {
        dd_strbuf bar;
        double pct = total_files > 0
                         ? 100.0 * (double)ar->b[i].files / (double)total_files
                         : 0.0;
        dd_sb_init(&bar);
        dd_bar(&bar, pct / 100.0, 10);
        if (o->bytes_mode) {
            dd_comma(ar->b[i].bytes, hsize, sizeof(hsize));
        } else {
            dd_human(ar->b[i].bytes, sm, hsize, sizeof(hsize));
        }
        dd_fprintf(out, "  %-12s  %8s  %12s  %6.1f%%  %s%s%s\n",
                   age_bucket_name(i),
                   dd_comma(ar->b[i].files, nbuf, sizeof(nbuf)),
                   hsize, pct,
                   dd_c(DD_C_DIM), bar.data, dd_c(DD_C_RESET));
        dd_sb_free(&bar);
    }

    if (ar->newest_path) {
        dd_fmt_time(ar->newest_mtime, tbuf, sizeof(tbuf));
        dd_fmt_age(ar->now - ar->newest_mtime, abuf, sizeof(abuf));
        dd_fprintf(out, "%sNewest:%s %s%s%s (%s, %s ago)\n",
                   dd_c(DD_C_BOLD), dd_c(DD_C_RESET),
                   dd_c(DD_C_CYAN), ar->newest_path, dd_c(DD_C_RESET),
                   tbuf, abuf);
    }
    if (ar->oldest_path) {
        dd_fmt_time(ar->oldest_mtime, tbuf, sizeof(tbuf));
        dd_fmt_age(ar->now - ar->oldest_mtime, abuf, sizeof(abuf));
        dd_fprintf(out, "%sOldest:%s %s%s%s (%s, %s ago)\n",
                   dd_c(DD_C_BOLD), dd_c(DD_C_RESET),
                   dd_c(DD_C_CYAN), ar->oldest_path, dd_c(DD_C_RESET),
                   tbuf, abuf);
    }
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

static void json_file_ref(FILE *out, const char *key, const char *path,
                          int64_t mtime)
{
    char tbuf[32];
    if (!path) {
        dd_fprintf(out, "\"%s\":null", key);
        return;
    }
    dd_fmt_time(mtime, tbuf, sizeof(tbuf));
    dd_fprintf(out, "\"%s\":{\"path\":", key);
    dd_json_quote(out, path);
    dd_fprintf(out, ",\"mtime\":%lld,\"mtime_iso\":\"%s\"}", (long long)mtime, tbuf);
}

int ages_json(const age_report *ar, const char *rootpath,
              const ages_options *o, FILE *out)
{
    int i;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32];

    dd_fprintf(out, "{\"path\":");
    dd_json_quote(out, rootpath);
    dd_fprintf(out, ",\"now\":%lld,\"buckets\":[", (long long)ar->now);
    for (i = 0; i < DD_AGE_BUCKETS; i++) {
        dd_human(ar->b[i].bytes, sm, hsize, sizeof(hsize));
        if (i > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"name\":\"%s\",\"files\":%llu,\"bytes\":%llu,\"human\":\"%s\"}",
                   age_bucket_name(i),
                   (unsigned long long)ar->b[i].files,
                   (unsigned long long)ar->b[i].bytes, hsize);
    }
    DD_FPUTS("],", out);
    json_file_ref(out, "newest", ar->newest_path, ar->newest_mtime);
    DD_FPUTC(',', out);
    json_file_ref(out, "oldest", ar->oldest_path, ar->oldest_mtime);
    DD_FPUTC('}', out);
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* ages command                                                        */
/* ------------------------------------------------------------------ */

enum { DD_AGES_BYTES = 1, DD_AGES_FOLLOW };

static const dd_option g_ages_opts[] = {
    { "bytes", 'b', 0, DD_AGES_BYTES, NULL,
      "print raw byte counts instead of human units" },
    { "follow", 'L', 0, DD_AGES_FOLLOW, NULL,
      "follow symbolic links (default: never)" }
};

static void ages_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s ages [PATH] [options]\n\n"
        "Group files into five fixed age buckets by modification time:\n"
        "<1 day, 1-7 days, 7-30 days, 30-365 days, >1 year. The newest and\n"
        "oldest files are highlighted at the end.\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_ages_opts, DD_NELTS(g_ages_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --json --si --color --no-color -h\n"
             "\nExamples:\n"
             "  diskdive ages /var/log\n"
             "  diskdive ages . --json\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int ages_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    ages_options o;
    scan_options so;
    age_report ar;
    scan_stats st;
    dd_args a;
    const char *path;
    char err[256];
    size_t i;
    int follow = 0, rc;

    memset(&o, 0, sizeof(o));

    if (dd_args_parse(argc - 1, argv + 1, g_ages_opts, DD_NELTS(g_ages_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("ages", err, g_ages_opts, DD_NELTS(g_ages_opts));
    }
    if (g->help) {
        ages_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (a.npos > 1) {
        dd_usage_error("ages", "too many arguments", g_ages_opts, DD_NELTS(g_ages_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_AGES_BYTES:
            o.bytes_mode = 1;
            break;
        case DD_AGES_FOLLOW:
            follow = 1;
            break;
        default:
            break;
        }
    }

    o.si = g->si;
    scan_options_defaults(&so);
    so.follow_symlinks = follow;

    scan_stats_init(&st);
    rc = ages_collect(path, &so, &ar, &st, err, sizeof(err));
    if (rc != 0) {
        dd_error("%s", err);
        dd_args_free(&a);
        return DD_EXIT_RUNTIME;
    }
    rc = g->json ? ages_json(&ar, path, &o, stdout)
                 : ages_print(&ar, path, &o, stdout);
    ages_free(&ar);
    dd_args_free(&a);
    if (rc != 0 || !dd_out_ok(stdout)) {
        if (!dd_out_ok(stdout)) {
            dd_error("write error");
        }
        return DD_EXIT_RUNTIME;
    }
    return DD_EXIT_OK;
}
