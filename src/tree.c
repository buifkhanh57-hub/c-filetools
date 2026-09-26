/*
 * tree.c — box-drawing tree printer on top of the du aggregation tree.
 */
#include "args.h"
#include "tree.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Entry view (mixes directories and files of one parent)              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int is_dir;
    uint64_t size;      /* du_node_size for dirs, st_size for files */
    const du_node *node; /* NULL for plain files                    */
} tview;

static int g_tkey = DD_TREESORT_NAME_ASC;

static int tview_cmp(const void *a, const void *b)
{
    const tview *ta = a;
    const tview *tb = b;
    int c;

    if (g_tkey == DD_TREESORT_SIZE_DESC) {
        if (ta->size != tb->size) {
            return ta->size > tb->size ? -1 : 1;
        }
    } else {
        /* directories first, then alphabetical */
        if (ta->is_dir != tb->is_dir) {
            return ta->is_dir > tb->is_dir ? -1 : 1;
        }
    }
    c = strcmp(ta->name, tb->name);
    return c;
}

static size_t collect_children(const du_node *n, tview **out, const tree_options *o)
{
    size_t i, len = 0;
    tview *arr = dd_xmalloc((n->nchildren + n->nfiles + 1) * sizeof(*arr));

    for (i = 0; i < n->nchildren; i++) {
        const du_node *ch = n->children[i];
        if (!o->all && dd_name_is_hidden(ch->name)) {
            continue;
        }
        arr[len].name = ch->name;
        arr[len].is_dir = 1;
        arr[len].size = ch->total_bytes;
        arr[len].node = ch;
        len++;
    }
    for (i = 0; i < n->nfiles; i++) {
        if (!o->all && dd_name_is_hidden(n->files[i].name)) {
            continue;
        }
        arr[len].name = n->files[i].name;
        arr[len].is_dir = 0;
        arr[len].size = n->files[i].size;
        arr[len].node = NULL;
        len++;
    }
    g_tkey = o->sort_key;
    qsort(arr, len, sizeof(*arr), tview_cmp);
    *out = arr;
    return len;
}

/* ------------------------------------------------------------------ */
/* Recursive printer                                                   */
/* ------------------------------------------------------------------ */

static void print_node(const du_node *n, const char *rootpath, dd_strbuf *prefix,
                       int depth, const tree_options *o, FILE *out)
{
    tview *kids = NULL;
    size_t nkids = collect_children(n, &kids, o);
    size_t i;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32];

    for (i = 0; i < nkids; i++) {
        const tview *t = &kids[i];
        int last = (i + 1 == nkids);
        const char *conn = o->ascii ? (last ? "`-- " : "|-- ")
                                    : (last ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 "
                                            : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ");
        const char *pipe = o->ascii ? (last ? "    " : "|   ")
                                    : (last ? "    " : "\xE2\x94\x82   ");
        int prune_here = (o->depth > 0 && depth + 1 >= o->depth && t->is_dir && t->node &&
                          (t->node->nchildren > 0 || t->node->nfiles > 0));

        DD_FPUTS(prefix->data, out);
        DD_FPUTS(conn, out);

        if (t->is_dir) {
            if (o->bytes_mode) {
                dd_comma(t->size, hsize, sizeof(hsize));
            } else {
                dd_human(t->size, sm, hsize, sizeof(hsize));
            }
            dd_fprintf(out, "%s%s%s/ %s[%s%s]%s\n",
                       dd_c(DD_C_BOLD), t->name, dd_c(DD_C_RESET),
                       dd_c(DD_C_DIM), hsize, prune_here ? ", ..." : "",
                       dd_c(DD_C_RESET));
            if (prune_here) {
                continue;
            }
            {
                size_t plen = prefix->len;
                dd_sb_append(prefix, pipe);
                print_node(t->node, rootpath, prefix, depth + 1, o, out);
                dd_sb_truncate(prefix, plen);
            }
        } else {
            if (o->bytes_mode) {
                dd_comma(t->size, hsize, sizeof(hsize));
            } else {
                dd_human(t->size, sm, hsize, sizeof(hsize));
            }
            dd_fprintf(out, "%s  %s%s%s\n", t->name, dd_c(DD_C_DIM), hsize,
                       dd_c(DD_C_RESET));
        }
    }
    free(kids);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int tree_run(const char *root, const scan_options *so, const tree_options *o,
             FILE *out)
{
    du_node *tree = NULL;
    du_options dopt;
    scan_stats st;
    char err[512];
    dd_strbuf prefix;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32], nbuf[32];

    memset(&dopt, 0, sizeof(dopt));
    dopt.keep_files = 1;

    scan_stats_init(&st);
    if (du_analyze(root, &dopt, so, &tree, &st, err, sizeof(err)) != 0) {
        dd_error("%s", err);
        return DD_EXIT_RUNTIME;
    }

    if (o->bytes_mode) {
        dd_comma(tree->total_bytes, hsize, sizeof(hsize));
    } else {
        dd_human(tree->total_bytes, sm, hsize, sizeof(hsize));
    }
    dd_fprintf(out, "%s%s%s %s[%s]%s\n",
               dd_c(DD_C_BOLD), root, dd_c(DD_C_RESET),
               dd_c(DD_C_DIM), hsize, dd_c(DD_C_RESET));

    dd_sb_init(&prefix);
    print_node(tree, root, &prefix, 0, o, out);
    dd_sb_free(&prefix);

    dd_fprintf(out, "\n%llu director%s, %s file%s\n",
               (unsigned long long)tree->dir_count,
               tree->dir_count == 1 ? "y" : "ies",
               dd_comma((uint64_t)tree->file_count, nbuf, sizeof(nbuf)),
               tree->file_count == 1 ? "" : "s");
    if (st.errors > 0) {
        dd_fprintf(out, "%s%llu entr%s could not be read%s\n",
                   dd_c(DD_C_RED), (unsigned long long)st.errors,
                   st.errors == 1 ? "y" : "ies", dd_c(DD_C_RESET));
    }

    du_free(tree);
    return dd_out_ok(out) ? DD_EXIT_OK : DD_EXIT_RUNTIME;
}

/* ------------------------------------------------------------------ */
/* tree command                                                        */
/* ------------------------------------------------------------------ */

enum { DD_TREE_DEPTH = 1, DD_TREE_ALL, DD_TREE_ASCII, DD_TREE_SORT,
       DD_TREE_BYTES, DD_TREE_FOLLOW };

static const dd_option g_tree_opts[] = {
    { "depth", 'd', 1, DD_TREE_DEPTH, "N",
      "limit the printed depth (0 = unlimited, default 3)" },
    { "all", 'a', 0, DD_TREE_ALL, NULL,
      "include hidden entries (excluded by default)" },
    { "ascii", 'A', 0, DD_TREE_ASCII, NULL,
      "ASCII connectors (|-- `--) instead of Unicode box drawing" },
    { "sort", 's', 1, DD_TREE_SORT, "KEY",
      "order entries by name|size (default: name)" },
    { "bytes", 'b', 0, DD_TREE_BYTES, NULL,
      "print raw byte counts instead of human units" },
    { "follow", 'L', 0, DD_TREE_FOLLOW, NULL,
      "follow symbolic links (default: never)" }
};

static void tree_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s tree [PATH] [options]\n\n"
        "Draw a depth-limited directory tree with aggregated sizes.\n"
        "Directory sizes are exact (full-depth walk); --depth only limits\n"
        "the drawing, frontier directories carry a \"...\" marker.\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_tree_opts, DD_NELTS(g_tree_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --si --color --no-color -h\n"
             "Note: --json is not supported by tree.\n"
             "\nExamples:\n"
             "  diskdive tree src --depth 2\n"
             "  diskdive tree . --all --sort size\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int tree_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    tree_options o;
    scan_options so;
    dd_args a;
    const char *path;
    char err[256];
    size_t i;
    int follow = 0, rc;

    memset(&o, 0, sizeof(o));
    o.depth = 3;
    o.sort_key = DD_TREESORT_NAME_ASC;

    if (dd_args_parse(argc - 1, argv + 1, g_tree_opts, DD_NELTS(g_tree_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("tree", err, g_tree_opts, DD_NELTS(g_tree_opts));
    }
    if (g->help) {
        tree_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (g->json) {
        dd_args_free(&a);
        dd_usage_error("tree", "--json is not supported for 'tree'",
                       g_tree_opts, DD_NELTS(g_tree_opts));
    }
    if (a.npos > 1) {
        dd_usage_error("tree", "too many arguments", g_tree_opts, DD_NELTS(g_tree_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_TREE_DEPTH: {
            size_t v;
            if (dd_parse_count(p->val, &v, err, sizeof(err)) != 0) {
                dd_usage_error("tree", err, g_tree_opts, DD_NELTS(g_tree_opts));
            }
            o.depth = (int)v;
            break;
        }
        case DD_TREE_ALL:
            o.all = 1;
            break;
        case DD_TREE_ASCII:
            o.ascii = 1;
            break;
        case DD_TREE_SORT:
            if (strcmp(p->val, "name") == 0) {
                o.sort_key = DD_TREESORT_NAME_ASC;
            } else if (strcmp(p->val, "size") == 0) {
                o.sort_key = DD_TREESORT_SIZE_DESC;
            } else {
                snprintf(err, sizeof(err),
                         "invalid --sort key '%s' (expected name|size)", p->val);
                dd_usage_error("tree", err, g_tree_opts, DD_NELTS(g_tree_opts));
            }
            break;
        case DD_TREE_BYTES:
            o.bytes_mode = 1;
            break;
        case DD_TREE_FOLLOW:
            follow = 1;
            break;
        default:
            break;
        }
    }

    o.si = g->si;
    scan_options_defaults(&so);
    so.follow_symlinks = follow;
    so.include_hidden = o.all;

    rc = tree_run(path, &so, &o, stdout);
    dd_args_free(&a);
    if (!dd_out_ok(stdout)) {
        dd_error("write error");
        return DD_EXIT_RUNTIME;
    }
    return rc;
}
