/*
 * clean.c — candidate discovery, dry-run output and safe deletion.
 */
#include "args.h"
#include "clean.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Built-in rules                                                      */
/* ------------------------------------------------------------------ */

void clean_options_defaults(clean_options *o)
{
    dd_sl_init(&o->file_globs);
    dd_sl_init(&o->dir_names);
    o->older_than_days = 0;
    o->follow = 0;

    dd_sl_add(&o->file_globs, "*.tmp");
    dd_sl_add(&o->file_globs, "*.temp");
    dd_sl_add(&o->file_globs, "*.swp");
    dd_sl_add(&o->file_globs, "*.swo");
    dd_sl_add(&o->file_globs, "*.bak");
    dd_sl_add(&o->file_globs, "*~");
    dd_sl_add(&o->file_globs, "*.log");
    dd_sl_add(&o->file_globs, ".DS_Store");

    dd_sl_add(&o->dir_names, "__pycache__");
    dd_sl_add(&o->dir_names, "node_modules");
    dd_sl_add(&o->dir_names, ".cache");
    dd_sl_add(&o->dir_names, ".pytest_cache");
    dd_sl_add(&o->dir_names, ".mypy_cache");
}

void clean_options_free(clean_options *o)
{
    dd_sl_free(&o->file_globs);
    dd_sl_free(&o->dir_names);
}

/* scan policy for clean walks: hidden entries must be visible so that
 * .DS_Store / .cache can be found; symlinks only when --follow. */
static void clean_scan_options(const clean_options *o, scan_options *so)
{
    scan_options_defaults(so);
    so->follow_symlinks = o->follow;
    so->include_hidden = 1;
}

/* ------------------------------------------------------------------ */
/* Candidate collection                                                */
/* ------------------------------------------------------------------ */

static uint64_t dir_size_recursive(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    uint64_t total = 0;
    dd_strbuf sb;

    if (!d) {
        return 0;
    }
    dd_sb_init(&sb);
    while ((de = readdir(d)) != NULL) {
        struct stat st;
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
            continue;
        }
        dd_sb_reset(&sb);
        dd_sb_append(&sb, path);
        if (sb.len == 0 || sb.data[sb.len - 1] != '/') {
            dd_sb_appendch(&sb, '/');
        }
        dd_sb_append(&sb, de->d_name);
        if (lstat(sb.data, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            total += dir_size_recursive(sb.data);
        } else if (S_ISREG(st.st_mode)) {
            total += (uint64_t)st.st_size;
        }
    }
    dd_sb_free(&sb);
    closedir(d);
    return total;
}

typedef struct {
    clean_list *list;
    const clean_options *o;
    int64_t now;
} clean_ctx;

static void cl_push(clean_list *l, const char *path, uint64_t bytes,
                    int is_dir, const char *reason, int age_days)
{
    clean_item *it;
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = dd_xrealloc(l->items, l->cap * sizeof(*l->items));
    }
    it = &l->items[l->len++];
    it->path = dd_xstrdup(path);
    it->bytes = bytes;
    it->is_dir = is_dir;
    it->reason = dd_xstrdup(reason);
    it->age_days = age_days;
    l->total_bytes += bytes;
}

static int clean_on_dir_pre(void *ud, const char *path, const struct stat *st, int depth)
{
    clean_ctx *c = ud;
    const char *name = dd_basename(path);
    (void)st;
    (void)depth;

    if (!dd_sl_contains(&c->o->dir_names, name)) {
        return 0; /* not junk: descend */
    }
    cl_push(c->list, path, dir_size_recursive(path), 1, "cache directory", -1);
    return 1; /* skip the subtree entirely */
}

static void clean_on_file(void *ud, const char *path, const struct stat *st, int depth)
{
    clean_ctx *c = ud;
    const char *name = dd_basename(path);
    size_t gi;
    int64_t age;
    char reason[96];
    (void)depth;

    for (gi = 0; gi < c->o->file_globs.len; gi++) {
        if (fnmatch(c->o->file_globs.items[gi], name, 0) == 0) {
            break;
        }
    }
    if (gi == c->o->file_globs.len) {
        return; /* no pattern matched */
    }
    age = (c->now - (int64_t)st->st_mtime) / (24 * 60 * 60);
    if (age < 0) {
        age = 0;
    }
    if (c->o->older_than_days > 0 && age < c->o->older_than_days) {
        return; /* too fresh */
    }
    snprintf(reason, sizeof(reason), "matched %s", c->o->file_globs.items[gi]);
    cl_push(c->list, path, (uint64_t)st->st_size, 0, reason, (int)age);
}

int clean_collect(const char *root, const clean_options *o, clean_list *out,
                  scan_stats *st, char *err, size_t errn)
{
    scan_visitor v;
    clean_ctx c;
    scan_options so;

    memset(out, 0, sizeof(*out));
    c.list = out;
    c.o = o;
    c.now = (int64_t)time(NULL);

    memset(&v, 0, sizeof(v));
    v.ud = &c;
    v.on_file = clean_on_file;
    v.on_dir_pre = clean_on_dir_pre;

    clean_scan_options(o, &so);
    return scan_walk(root, &so, &v, st, err, errn);
}

void clean_list_free(clean_list *l)
{
    size_t i;
    for (i = 0; i < l->len; i++) {
        free(l->items[i].path);
        free(l->items[i].reason);
    }
    free(l->items);
    memset(l, 0, sizeof(*l));
}

/* ------------------------------------------------------------------ */
/* Recursive deletion (never follows symlinks)                         */
/* ------------------------------------------------------------------ */

static int rm_rf(const char *path, uint64_t *freed, size_t *errors)
{
    struct stat st;
    DIR *d;
    struct dirent *de;
    dd_strbuf sb;
    size_t base_len;

    if (lstat(path, &st) != 0) {
        (*errors)++;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        /* file, symlink, fifo ...: unlink directly, never descend */
        uint64_t sz = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
        if (unlink(path) != 0) {
            (*errors)++;
            return -1;
        }
        *freed += sz;
        return 0;
    }

    d = opendir(path);
    if (!d) {
        (*errors)++;
        return -1;
    }
    dd_sb_init(&sb);
    dd_sb_append(&sb, path);
    base_len = sb.len;

    errno = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
            continue;
        }
        dd_sb_truncate(&sb, base_len);
        if (sb.len == 0 || sb.data[sb.len - 1] != '/') {
            dd_sb_appendch(&sb, '/');
        }
        dd_sb_append(&sb, de->d_name);
        rm_rf(sb.data, freed, errors);
        errno = 0;
    }
    dd_sb_free(&sb);
    closedir(d);
    if (rmdir(path) != 0) {
        (*errors)++;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

int clean_print(const clean_list *l, const char *rootpath,
                int bytes_mode, int si, int is_dryrun, FILE *out)
{
    dd_size_mode sm = si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32], abuf[32];
    size_t i;

    dd_fprintf(out, "%sClean candidates in %s%s\n",
               dd_c(DD_C_BOLD), rootpath, dd_c(DD_C_RESET));
    if (is_dryrun) {
        dd_fprintf(out, "%sDRY RUN — nothing will be deleted%s\n",
                   dd_c(DD_C_YELLOW), dd_c(DD_C_RESET));
    }
    if (l->len == 0) {
        DD_FPUTS("No clean candidates found.\n", out);
        return 0;
    }
    for (i = 0; i < l->len; i++) {
        const clean_item *it = &l->items[i];
        if (bytes_mode) {
            dd_comma(it->bytes, hsize, sizeof(hsize));
        } else {
            dd_human(it->bytes, sm, hsize, sizeof(hsize));
        }
        dd_fprintf(out, "  [%s] %12s  %s%s%s",
                   it->is_dir ? "dir " : "file",
                   hsize,
                   dd_c(DD_C_CYAN), it->path, dd_c(DD_C_RESET));
        if (it->age_days >= 0) {
            snprintf(abuf, sizeof(abuf), "%dd", it->age_days);
            dd_fprintf(out, "  %s(%s, %s old)%s", dd_c(DD_C_DIM), it->reason, abuf,
                       dd_c(DD_C_RESET));
        } else {
            dd_fprintf(out, "  %s(%s)%s", dd_c(DD_C_DIM), it->reason, dd_c(DD_C_RESET));
        }
        DD_FPUTC('\n', out);
    }
    dd_fprintf(out, "%llu item%s · %s reclaimable\n",
               (unsigned long long)l->len, l->len == 1 ? "" : "s",
               bytes_mode ? dd_comma(l->total_bytes, hsize, sizeof(hsize))
                          : dd_human(l->total_bytes, sm, hsize, sizeof(hsize)));
    if (is_dryrun) {
        DD_FPUTS("Re-run with --delete to remove these items.\n", out);
    }
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

int clean_execute(const char *rootpath, const clean_list *l,
                  int assume_yes, size_t *deleted, uint64_t *freed,
                  size_t *errors, FILE *in, FILE *out)
{
    char *realbuf;
    char line[64];
    size_t i;
    dd_size_mode sm = DD_SIZE_BIN;
    char hsize[32];

    *deleted = 0;
    *freed = 0;
    *errors = 0;

    /* safety: never delete the filesystem root */
    realbuf = realpath(rootpath, NULL);
    if (realbuf) {
        int is_root = (strcmp(realbuf, "/") == 0);
        free(realbuf);
        if (is_root) {
            dd_error("refusing to clean the filesystem root");
            return DD_EXIT_RUNTIME;
        }
    }

    if (!assume_yes) {
        if (!isatty(fileno(in))) {
            dd_error("refusing to delete without --yes when input is not a terminal");
            return DD_EXIT_RUNTIME;
        }
        dd_fprintf(out, "Delete %llu item%s (%s) under %s? Type 'yes' to confirm: ",
                   (unsigned long long)l->len, l->len == 1 ? "" : "s",
                   dd_human(l->total_bytes, sm, hsize, sizeof(hsize)), rootpath);
        fflush(out);
        if (!fgets(line, sizeof(line), in)) {
            DD_FPUTS("\nAborted — nothing deleted.\n", out);
            return DD_EXIT_RUNTIME;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "yes") != 0 && strcmp(line, "y") != 0) {
            dd_fprintf(out, "Aborted — nothing deleted.\n");
            return DD_EXIT_RUNTIME;
        }
    }

    for (i = 0; i < l->len; i++) {
        const clean_item *it = &l->items[i];
        size_t errs_before = *errors;
        rm_rf(it->path, freed, errors);
        if (*errors == errs_before) {
            (*deleted)++;
        }
    }

    dd_fprintf(out, "Deleted %llu item%s · freed %s · %llu error%s\n",
               (unsigned long long)*deleted, *deleted == 1 ? "" : "s",
               dd_human(*freed, sm, hsize, sizeof(hsize)),
               (unsigned long long)*errors, *errors == 1 ? "" : "s");
    return *errors > 0 ? DD_EXIT_RUNTIME : DD_EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* clean command                                                       */
/* ------------------------------------------------------------------ */

enum { DD_CLEAN_DELETE = 1, DD_CLEAN_YES, DD_CLEAN_OLDER, DD_CLEAN_GLOB,
       DD_CLEAN_DIR, DD_CLEAN_BYTES, DD_CLEAN_FOLLOW };

static const dd_option g_clean_opts[] = {
    { "delete", 'd', 0, DD_CLEAN_DELETE, NULL,
      "actually delete the candidates (default: dry run)" },
    { "yes", 'y', 0, DD_CLEAN_YES, NULL,
      "skip the typed confirmation (for scripts, use with care)" },
    { "older-than", 'o', 1, DD_CLEAN_OLDER, "DAYS",
      "only flag files at least this old (0 = any age, default 0)" },
    { "glob", 'g', 1, DD_CLEAN_GLOB, "PATTERN",
      "add a file pattern to the built-in rules (repeatable)" },
    { "dir", 'r', 1, DD_CLEAN_DIR, "NAME",
      "add a cache directory name to the built-in rules (repeatable)" },
    { "bytes", 'b', 0, DD_CLEAN_BYTES, NULL,
      "print raw byte counts instead of human units" },
    { "follow", 'L', 0, DD_CLEAN_FOLLOW, NULL,
      "follow symlinked directories while scanning (default: never)" }
};

static void clean_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s clean [PATH] [options]\n\n"
        "Find cache/tmp junk: *.tmp *.temp *.swp *.swo *.bak *~ *.log\n"
        ".DS_Store files and __pycache__/node_modules/.cache/.pytest_cache/\n"
        ".mypy_cache directories (see --glob/--dir to extend the rules).\n\n"
        "SAFETY: without --delete this is a DRY RUN — nothing is removed.\n"
        "With --delete, a typed confirmation is required on a terminal;\n"
        "scripts must pass --yes explicitly. '/' is never accepted.\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_clean_opts, DD_NELTS(g_clean_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --si --color --no-color -h\n"
             "Note: --json is not supported by clean.\n"
             "\nExamples:\n"
             "  diskdive clean .\n"
             "  diskdive clean /tmp --older-than 7\n"
             "  diskdive clean . --delete --yes\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int clean_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    clean_options o;
    scan_stats st;
    clean_list list;
    dd_args a;
    const char *path;
    char err[256];
    size_t i;
    int follow = 0, want_delete = 0, assume_yes = 0, bytes_mode = 0, rc;

    if (dd_args_parse(argc - 1, argv + 1, g_clean_opts, DD_NELTS(g_clean_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("clean", err, g_clean_opts, DD_NELTS(g_clean_opts));
    }
    if (g->help) {
        clean_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (g->json) {
        dd_args_free(&a);
        dd_usage_error("clean", "--json is not supported for 'clean'",
                       g_clean_opts, DD_NELTS(g_clean_opts));
    }
    if (a.npos > 1) {
        dd_usage_error("clean", "too many arguments", g_clean_opts, DD_NELTS(g_clean_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    clean_options_defaults(&o);
    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_CLEAN_DELETE:
            want_delete = 1;
            break;
        case DD_CLEAN_YES:
            assume_yes = 1;
            break;
        case DD_CLEAN_OLDER: {
            size_t v;
            if (dd_parse_count(p->val, &v, err, sizeof(err)) != 0) {
                dd_usage_error("clean", err, g_clean_opts, DD_NELTS(g_clean_opts));
            }
            o.older_than_days = (int)v;
            break;
        }
        case DD_CLEAN_GLOB:
            dd_sl_add(&o.file_globs, p->val);
            break;
        case DD_CLEAN_DIR:
            dd_sl_add(&o.dir_names, p->val);
            break;
        case DD_CLEAN_BYTES:
            bytes_mode = 1;
            break;
        case DD_CLEAN_FOLLOW:
            follow = 1;
            break;
        default:
            break;
        }
    }
    o.follow = follow;

    scan_stats_init(&st);
    rc = clean_collect(path, &o, &list, &st, err, sizeof(err));
    if (rc != 0) {
        dd_error("%s", err);
        clean_options_free(&o);
        dd_args_free(&a);
        return DD_EXIT_RUNTIME;
    }

    rc = clean_print(&list, path, bytes_mode, g->si, !want_delete, stdout);
    if (rc == 0 && want_delete && list.len > 0) {
        size_t deleted = 0, errors = 0;
        uint64_t freed = 0;
        rc = clean_execute(path, &list, assume_yes, &deleted, &freed, &errors,
                           stdin, stdout);
    }
    clean_list_free(&list);
    clean_options_free(&o);
    dd_args_free(&a);
    if (!dd_out_ok(stdout)) {
        dd_error("write error");
        return DD_EXIT_RUNTIME;
    }
    return rc == 0 ? DD_EXIT_OK : rc;
}
