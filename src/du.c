/*
 * du.c — build and print the directory aggregation tree.
 */
#include "args.h"
#include "du.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tree construction                                                   */
/* ------------------------------------------------------------------ */

static du_node *node_new(const char *name, du_node *parent, int depth)
{
    du_node *n = dd_xcalloc(1, sizeof(*n));
    n->name = dd_xstrdup(name);
    n->parent = parent;
    n->depth = depth;
    return n;
}

static du_node *node_add_child(du_node *parent, const char *name, int depth)
{
    du_node *child;
    if (parent->nchildren == parent->childcap) {
        parent->childcap = parent->childcap ? parent->childcap * 2 : 8;
        parent->children = dd_xrealloc(parent->children,
                                        parent->childcap * sizeof(*parent->children));
    }
    child = node_new(name, parent, depth);
    parent->children[parent->nchildren++] = child;
    return child;
}

static void node_add_file(du_node *n, const char *path, const struct stat *st)
{
    if (n->nfiles == n->filecap) {
        n->filecap = n->filecap ? n->filecap * 2 : 16;
        n->files = dd_xrealloc(n->files, n->filecap * sizeof(*n->files));
    }
    n->files[n->nfiles].name = dd_xstrdup(dd_basename(path));
    n->files[n->nfiles].size = (uint64_t)st->st_size;
    n->nfiles++;
}

typedef struct {
    du_node *cur;
    const du_options *o;
} du_ctx;

static void du_on_file(void *ud, const char *path, const struct stat *st, int depth)
{
    du_ctx *c = ud;
    (void)depth;
    c->cur->own_bytes += (uint64_t)st->st_size;
    c->cur->own_blocks += (uint64_t)st->st_blocks * 512u;
    c->cur->file_count++;
    if (c->o->keep_files) {
        node_add_file(c->cur, path, st);
    }
}

static int du_on_dir_pre(void *ud, const char *path, const struct stat *st, int depth)
{
    du_ctx *c = ud;
    (void)st;
    /* the scanner passes the *parent's* walk depth here; the du tree
     * defines node depth as parent->depth + 1 (root = 0), which is
     * what --depth limits and the JSON emitter rely on. */
    (void)depth;
    c->cur = node_add_child(c->cur, dd_basename(path), c->cur->depth + 1);
    return 0;
}

static void du_on_dir_post(void *ud, const char *path, int depth)
{
    du_ctx *c = ud;
    du_node *n = c->cur;
    (void)path;
    (void)depth;

    if (n->parent) {
        /* roll this node up into its parent (post-order) */
        n->total_bytes += n->own_bytes;
        n->total_blocks += n->own_blocks;
        n->parent->total_bytes += n->total_bytes;
        n->parent->total_blocks += n->total_blocks;
        n->parent->file_count += n->file_count;
        n->parent->dir_count += n->dir_count + 1;
    }
    /* the scan root has no parent: du_analyze() rolls it up exactly
     * once after the walk (scan_walk fires on_dir_post for it too). */
    c->cur = n->parent;
}

int du_analyze(const char *root, const du_options *o, const scan_options *so,
               du_node **out, scan_stats *st, char *err, size_t errn)
{
    du_ctx c;
    scan_visitor v;
    du_node *tree;
    const char *name;

    /* the root node's display name: basename, or "/" when root is "/" */
    name = dd_basename(root);
    if (strcmp(root, "/") == 0 || (strcmp(name, "") == 0)) {
        name = "/";
    }
    tree = node_new(name, NULL, 0);

    c.cur = tree;
    c.o = o;

    memset(&v, 0, sizeof(v));
    v.ud = &c;
    v.on_file = du_on_file;
    v.on_dir_pre = du_on_dir_pre;
    v.on_dir_post = du_on_dir_post;

    if (scan_walk(root, so, &v, st, err, errn) != 0) {
        du_free(tree);
        return -1;
    }
    /* roll up the root itself */
    tree->total_bytes += tree->own_bytes;
    tree->total_blocks += tree->own_blocks;
    *out = tree;
    return 0;
}

void du_free(du_node *root)
{
    size_t i;
    if (!root) {
        return;
    }
    for (i = 0; i < root->nchildren; i++) {
        du_free(root->children[i]);
    }
    free(root->children);
    for (i = 0; i < root->nfiles; i++) {
        free(root->files[i].name);
    }
    free(root->files);
    free(root->name);
    free(root);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

uint64_t du_node_size(const du_node *n, int apparent)
{
    return apparent ? n->total_bytes : n->total_blocks;
}

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

static int g_dukey = DD_DUSORT_SIZE_DESC;
static int g_duapp = 0;

static int du_child_cmp(const void *a, const void *b)
{
    const du_node *const *pa = a;
    const du_node *const *pb = b;
    const du_node *na = *pa;
    const du_node *nb = *pb;

    switch (g_dukey) {
    case DD_DUSORT_NAME_ASC:
        return strcmp(na->name, nb->name);
    case DD_DUSORT_FILES_DESC:
        if (na->file_count != nb->file_count) {
            return na->file_count > nb->file_count ? -1 : 1;
        }
        break;
    default:
        if (du_node_size(na, g_duapp) != du_node_size(nb, g_duapp)) {
            return du_node_size(na, g_duapp) > du_node_size(nb, g_duapp) ? -1 : 1;
        }
        break;
    }
    return strcmp(na->name, nb->name);
}

/**
 * Snapshot of @p parent's direct children, sorted by @p key
 * (DD_DUSORT_*). The caller owns the returned array (free()) but not
 * the nodes inside it. Used by report for the "largest dirs" section
 * and by the JSON backend, which emits one nested level at a time
 * (unlike the flat du table).
 */
static void sort_children_snapshot(const du_node *parent, int key, int apparent,
                                   const du_node ***rows, size_t *nrows)
{
    size_t i;
    *nrows = 0;
    *rows = parent->nchildren
                ? dd_xmalloc(parent->nchildren * sizeof(**rows))
                : NULL;
    for (i = 0; i < parent->nchildren; i++) {
        (*rows)[i] = parent->children[i];
    }
    *nrows = parent->nchildren;
    g_dukey = key;
    g_duapp = apparent;
    qsort((void *)*rows, *nrows, sizeof(**rows), du_child_cmp);
}

void du_sort_children(const du_node *parent, int key, int apparent,
                      const du_node ***rows, size_t *nrows)
{
    sort_children_snapshot(parent, key, apparent, rows, nrows);
}

void du_node_path(const du_node *n, const char *rootpath, dd_strbuf *sb)
{
    const du_node *stack[256];
    size_t len = 0, i;
    const du_node *p;

    dd_sb_reset(sb);
    for (p = n; p && p->parent; p = p->parent) {
        if (len < 256) {
            stack[len++] = p;
        }
    }
    dd_sb_append(sb, rootpath);
    for (i = len; i > 0; i--) {
        if (!(sb->len == 1 && sb->data[0] == '/')) {
            dd_sb_appendch(sb, '/');
        }
        dd_sb_append(sb, stack[i - 1]->name);
    }
}

static void visible_collect(const du_node *n, const du_options *o,
                            const du_node ***arr, size_t *len, size_t *cap)
{
    size_t i;
    for (i = 0; i < n->nchildren; i++) {
        du_node *ch = n->children[i];
        if (o->depth == 0 || ch->depth <= o->depth) {
            if (*len == *cap) {
                *cap = *cap ? *cap * 2 : 64;
                *arr = dd_xrealloc((void *)*arr, *cap * sizeof(**arr));
            }
            (*arr)[(*len)++] = ch;
        }
        if (o->depth == 0 || ch->depth < o->depth) {
            visible_collect(ch, o, arr, len, cap);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

static const char *sort_name(int key)
{
    switch (key) {
    case DD_DUSORT_NAME_ASC: return "name";
    case DD_DUSORT_FILES_DESC: return "file count";
    default: return "size";
    }
}

int du_print(const du_node *root, const du_options *o, const char *rootpath,
             const scan_stats *st, FILE *out)
{
    const du_node **rows = NULL;
    size_t nrows = 0, ncap = 0;
    size_t i, shown;
    dd_strbuf pb;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32], htotal[32], htotal2[32], nfiles[32], ndirs[32];
    double total = (double)du_node_size(root, o->apparent);

    g_dukey = o->sort_key;
    g_duapp = o->apparent;
    visible_collect(root, o, &rows, &nrows, &ncap);
    qsort(rows, nrows, sizeof(*rows), du_child_cmp);

    dd_fprintf(out, "%sDisk usage for %s%s (%s, sorted by %s)\n",
               dd_c(DD_C_BOLD), rootpath, dd_c(DD_C_RESET),
               o->apparent ? "apparent sizes" : "on-disk sizes",
               sort_name(o->sort_key));

    dd_sb_init(&pb);
    shown = (o->top > 0 && nrows > (size_t)o->top) ? (size_t)o->top : nrows;
    for (i = 0; i < shown; i++) {
        const du_node *n = rows[i];
        dd_strbuf bar;
        double frac = total > 0.0 ? (double)du_node_size(n, o->apparent) / total : 0.0;

        du_node_path(n, rootpath, &pb);
        dd_sb_init(&bar);
        dd_bar(&bar, frac, 10);

        if (o->bytes_mode) {
            dd_comma(du_node_size(n, o->apparent), hsize, sizeof(hsize));
        } else {
            dd_human(du_node_size(n, o->apparent), sm, hsize, sizeof(hsize));
        }
        dd_comma(n->file_count, nfiles, sizeof(nfiles));

        dd_fprintf(out, "%10s  %6.1f%%  %8s  %s%s%s  %s%s%s\n",
                   hsize, frac * 100.0, nfiles,
                   dd_c(DD_C_DIM), bar.data, dd_c(DD_C_RESET),
                   dd_c(DD_C_CYAN), pb.data, dd_c(DD_C_RESET));
        dd_sb_free(&bar);
    }
    if (shown < nrows) {
        dd_fprintf(out, "  ... %s more directories (use --top 0 to show all)\n",
                   dd_comma(nrows - shown, nfiles, sizeof(nfiles)));
    }

    dd_human(du_node_size(root, o->apparent), sm, htotal, sizeof(htotal));
    dd_human(du_node_size(root, !o->apparent), sm, htotal2, sizeof(htotal2));
    dd_fprintf(out, "%sTotal:%s %s (%s apparent) · %s files · %s directories\n",
               dd_c(DD_C_BOLD), dd_c(DD_C_RESET),
               o->bytes_mode
                   ? dd_comma(du_node_size(root, o->apparent), htotal, sizeof(htotal))
                   : htotal,
               htotal2,
               dd_comma(root->file_count, nfiles, sizeof(nfiles)),
               dd_comma(root->dir_count, ndirs, sizeof(ndirs)));
    if (st->errors > 0) {
        dd_fprintf(out, "%s%llu entr%s could not be read%s\n",
                   dd_c(DD_C_RED), (unsigned long long)st->errors,
                   st->errors == 1 ? "y" : "ies", dd_c(DD_C_RESET));
    }
    dd_sb_free(&pb);
    free((void *)rows);
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

static int json_depth_limit(const du_options *o)
{
    return o->depth > 0 ? o->depth : 1 << 30;
}

static void json_node(const du_node *n, const du_options *o, const char *rootpath,
                      FILE *out, int *first, int depth, int limit)
{
    size_t i;
    char hsize[32];
    dd_strbuf pb;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;

    if (!*first) {
        DD_FPUTS(",", out);
    }
    *first = 0;

    dd_sb_init(&pb);
    du_node_path(n, rootpath, &pb);
    dd_human(du_node_size(n, o->apparent), sm, hsize, sizeof(hsize));

    dd_fprintf(out, "{\"name\":");
    dd_json_quote(out, n->name);
    dd_fprintf(out, ",\"path\":");
    dd_json_quote(out, pb.data);
    dd_fprintf(out, ",\"bytes\":%llu,\"human\":\"%s\",\"files\":%llu,\"dirs\":%llu,\"children\":[",
               (unsigned long long)du_node_size(n, o->apparent), hsize,
               (unsigned long long)n->file_count, (unsigned long long)n->dir_count);
    dd_sb_free(&pb);

    if (depth < limit) {
        int cfirst = 1;
        const du_node **rows = NULL;
        size_t nrows = 0;
        /* exactly one nesting level: direct children, sorted; the
         * recursion emits their own nested arrays, so no directory is
         * ever repeated (a flattened walk would duplicate entries) */
        sort_children_snapshot(n, o->sort_key, o->apparent, &rows, &nrows);
        for (i = 0; i < nrows; i++) {
            json_node(rows[i], o, rootpath, out, &cfirst, depth + 1, limit);
        }
        free((void *)rows);
    }
    DD_FPUTS("]}", out);
}

int du_json(const du_node *root, const du_options *o, const char *rootpath,
            const scan_stats *st, FILE *out)
{
    char hsize[32];
    int first = 1;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    const du_node **rows = NULL;
    size_t nrows = 0, i;
    int limit = json_depth_limit(o);

    dd_human(du_node_size(root, o->apparent), sm, hsize, sizeof(hsize));
    dd_fprintf(out, "{\"path\":");
    dd_json_quote(out, rootpath);
    dd_fprintf(out, ",\"mode\":\"%s\",\"total_bytes\":%llu,\"total_human\":\"%s\","
                    "\"files\":%llu,\"dirs\":%llu,\"errors\":%llu,\"children\":[",
               o->apparent ? "apparent" : "on-disk",
               (unsigned long long)du_node_size(root, o->apparent), hsize,
               (unsigned long long)root->file_count,
               (unsigned long long)root->dir_count,
               (unsigned long long)st->errors);
    sort_children_snapshot(root, o->sort_key, o->apparent, &rows, &nrows);
    for (i = 0; i < nrows; i++) {
        json_node(rows[i], o, rootpath, out, &first, 1, limit);
    }
    free((void *)rows);
    DD_FPUTS("]}", out);
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* du command                                                          */
/* ------------------------------------------------------------------ */

enum {
    DD_DU_DEPTH = 1, DD_DU_SORT, DD_DU_TOP, DD_DU_APPARENT,
    DD_DU_BYTES, DD_DU_FOLLOW, DD_DU_ONE_FS
};

static const dd_option g_du_opts[] = {
    { "depth", 'd', 1, DD_DU_DEPTH, "N",
      "limit the printed depth (0 = unlimited, default 2)" },
    { "sort", 's', 1, DD_DU_SORT, "KEY",
      "sort rows by size|name|files (default: size)" },
    { "top", 'n', 1, DD_DU_TOP, "N",
      "print only the first N rows (0 = all, default 40)" },
    { "apparent", 'A', 0, DD_DU_APPARENT, NULL,
      "rank by apparent size (st_size) instead of on-disk blocks" },
    { "bytes", 'b', 0, DD_DU_BYTES, NULL,
      "print raw byte counts instead of human units" },
    { "follow", 'L', 0, DD_DU_FOLLOW, NULL,
      "follow symbolic links (default: never)" },
    { "one-file-system", 'x', 0, DD_DU_ONE_FS, NULL,
      "do not descend into other mount points" }
};

static void du_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s du [PATH] [options]\n\n"
        "Per-directory disk usage, sorted and human-readable. PATH defaults\n"
        "to \".\". The walk always covers the full tree; --depth only limits\n"
        "how many levels are printed, so totals stay exact.\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_du_opts, DD_NELTS(g_du_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --json --si --color --no-color -h\n"
             "\nExamples:\n"
             "  diskdive du /var --depth 1\n"
             "  diskdive du . --sort files --top 10\n"
             "  diskdive du --apparent --json\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int du_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    du_options o;
    scan_options so;
    dd_args a;
    scan_stats st;
    du_node *tree = NULL;
    const char *path;
    char err[256];
    size_t i;
    int follow = 0, one_fs = 0, rc;

    memset(&o, 0, sizeof(o));
    o.depth = 2;
    o.sort_key = DD_DUSORT_SIZE_DESC;
    o.top = 40;

    if (dd_args_parse(argc - 1, argv + 1, g_du_opts, DD_NELTS(g_du_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("du", err, g_du_opts, DD_NELTS(g_du_opts));
    }
    if (g->help) {
        du_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (a.npos > 1) {
        dd_usage_error("du", "too many arguments", g_du_opts, DD_NELTS(g_du_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_DU_DEPTH: {
            size_t v;
            if (dd_parse_count(p->val, &v, err, sizeof(err)) != 0) {
                dd_usage_error("du", err, g_du_opts, DD_NELTS(g_du_opts));
            }
            o.depth = (int)v;
            break;
        }
        case DD_DU_SORT:
            if (strcmp(p->val, "size") == 0) {
                o.sort_key = DD_DUSORT_SIZE_DESC;
            } else if (strcmp(p->val, "name") == 0) {
                o.sort_key = DD_DUSORT_NAME_ASC;
            } else if (strcmp(p->val, "files") == 0) {
                o.sort_key = DD_DUSORT_FILES_DESC;
            } else {
                snprintf(err, sizeof(err),
                         "invalid --sort key '%s' (expected size|name|files)", p->val);
                dd_usage_error("du", err, g_du_opts, DD_NELTS(g_du_opts));
            }
            break;
        case DD_DU_TOP: {
            size_t v;
            if (dd_parse_count(p->val, &v, err, sizeof(err)) != 0) {
                dd_usage_error("du", err, g_du_opts, DD_NELTS(g_du_opts));
            }
            o.top = (int)v;
            break;
        }
        case DD_DU_APPARENT:
            o.apparent = 1;
            break;
        case DD_DU_BYTES:
            o.bytes_mode = 1;
            break;
        case DD_DU_FOLLOW:
            follow = 1;
            break;
        case DD_DU_ONE_FS:
            one_fs = 1;
            break;
        default:
            break; /* global options already applied by the parser */
        }
    }

    scan_options_defaults(&so);
    so.follow_symlinks = follow;
    so.stay_on_fs = one_fs;
    so.include_hidden = 1;
    o.si = g->si;

    scan_stats_init(&st);
    if (du_analyze(path, &o, &so, &tree, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        dd_args_free(&a);
        return DD_EXIT_RUNTIME;
    }
    rc = g->json ? du_json(tree, &o, path, &st, stdout)
                 : du_print(tree, &o, path, &st, stdout);
    du_free(tree);
    dd_args_free(&a);
    if (rc != 0 || !dd_out_ok(stdout)) {
        if (!dd_out_ok(stdout)) {
            dd_error("write error");
        }
        return DD_EXIT_RUNTIME;
    }
    return DD_EXIT_OK;
}
