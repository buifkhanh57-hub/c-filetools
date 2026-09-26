/*
 * types.c — extension aggregation, category mapping and output.
 */
#include "args.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

static type_entry *map_find(type_map *tm, const char *ext)
{
    size_t i;
    for (i = 0; i < tm->len; i++) {
        if (strcmp(tm->items[i].ext, ext) == 0) {
            return &tm->items[i];
        }
    }
    return NULL;
}

typedef struct {
    type_map *tm;
} types_ctx;

static void types_on_file(void *ud, const char *path, const struct stat *st, int depth)
{
    types_ctx *c = ud;
    char ext[DD_TYPE_EXT_MAX];
    type_entry *e;
    (void)depth;

    dd_extension(path, ext, sizeof(ext));
    e = map_find(c->tm, ext);
    if (!e) {
        if (c->tm->len == c->tm->cap) {
            c->tm->cap = c->tm->cap ? c->tm->cap * 2 : 16;
            c->tm->items = dd_xrealloc(c->tm->items, c->tm->cap * sizeof(*c->tm->items));
        }
        e = &c->tm->items[c->tm->len++];
        memset(e, 0, sizeof(*e));
        snprintf(e->ext, sizeof(e->ext), "%s", ext);
    }
    e->bytes += (uint64_t)st->st_size;
    e->count++;
    c->tm->total_bytes += (uint64_t)st->st_size;
    c->tm->total_files++;
}

int types_collect(const char *root, const scan_options *so, type_map *tm,
                  scan_stats *st, char *err, size_t errn)
{
    scan_visitor v;
    types_ctx c;

    memset(tm, 0, sizeof(*tm));
    c.tm = tm;

    memset(&v, 0, sizeof(v));
    v.ud = &c;
    v.on_file = types_on_file;
    return scan_walk(root, so, &v, st, err, errn);
}

void types_free(type_map *tm)
{
    free(tm->items);
    memset(tm, 0, sizeof(*tm));
}

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

static int g_typekey = DD_TYPESORT_BYTES_DESC;

static int type_entry_cmp(const void *a, const void *b)
{
    const type_entry *pa = a;
    const type_entry *pb = b;

    switch (g_typekey) {
    case DD_TYPESORT_COUNT_DESC:
        if (pa->count != pb->count) {
            return pa->count > pb->count ? -1 : 1;
        }
        break;
    case DD_TYPESORT_NAME_ASC:
        return strcmp(pa->ext, pb->ext);
    default:
        if (pa->bytes != pb->bytes) {
            return pa->bytes > pb->bytes ? -1 : 1;
        }
        break;
    }
    return strcmp(pa->ext, pb->ext);
}

void types_sort(type_map *tm, int key)
{
    g_typekey = key;
    qsort(tm->items, tm->len, sizeof(*tm->items), type_entry_cmp);
}

/* ------------------------------------------------------------------ */
/* Category mapping                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *const *names;
    size_t n;
    const char *category;
} cat_rule;

static const char *const g_hdr_exts[] = { "h", "hh", "hpp", "hxx", "inl" };
static const char *const g_src_exts[] = {
    "c", "cpp", "cc", "cxx", "py", "js", "ts", "jsx", "tsx", "go", "rs",
    "java", "kt", "kts", "swift", "rb", "php", "pl", "sh", "bash", "zsh",
    "lua", "r", "dart", "cs", "m", "mm", "sql", "hs", "scala", "clj",
    "ex", "exs", "erl", "vim", "ps1", "bat", "asm", "s", "zig", "nim", "jl"
};
static const char *const g_doc_exts[] = {
    "md", "markdown", "rst", "txt", "tex", "adoc", "org", "doc", "docx",
    "odt", "rtf", "pdf", "epub", "mobi"
};
static const char *const g_data_exts[] = {
    "json", "yaml", "yml", "toml", "ini", "cfg", "conf", "xml", "csv",
    "tsv", "plist", "env", "properties", "sqlmap"
};
static const char *const g_img_exts[] = {
    "png", "jpg", "jpeg", "gif", "bmp", "svg", "webp", "ico", "tif",
    "tiff", "heic", "avif", "psd", "ai"
};
static const char *const g_audio_exts[] = { "mp3", "wav", "flac", "ogg", "m4a", "aac", "opus", "mid" };
static const char *const g_video_exts[] = { "mp4", "mkv", "avi", "mov", "webm", "flv", "wmv", "m4v" };
static const char *const g_arc_exts[] = {
    "zip", "tar", "gz", "bz2", "xz", "zst", "7z", "rar", "tgz", "tbz2", "txz", "lz4"
};
static const char *const g_bin_exts[] = {
    "exe", "dll", "so", "dylib", "a", "o", "obj", "bin", "elf", "app",
    "msi", "deb", "rpm", "jar", "class", "pyc", "pyo", "wasm", "node"
};
static const char *const g_font_exts[] = { "ttf", "otf", "woff", "woff2", "eot" };
static const char *const g_disk_exts[] = { "iso", "img", "dmg", "vhd", "qcow2" };

#define RULE(arr, cat) { arr, sizeof(arr) / sizeof((arr)[0]), cat }

static const cat_rule g_categories[] = {
    RULE(g_hdr_exts, "headers"),
    RULE(g_src_exts, "source"),
    RULE(g_doc_exts, "document"),
    RULE(g_data_exts, "data"),
    RULE(g_img_exts, "image"),
    RULE(g_audio_exts, "audio"),
    RULE(g_video_exts, "video"),
    RULE(g_arc_exts, "archive"),
    RULE(g_bin_exts, "binary"),
    RULE(g_font_exts, "font"),
    RULE(g_disk_exts, "disk image")
};

const char *types_category(const char *ext)
{
    size_t i, j;
    for (i = 0; i < sizeof(g_categories) / sizeof(g_categories[0]); i++) {
        const cat_rule *r = &g_categories[i];
        for (j = 0; j < r->n; j++) {
            if (strcmp(r->names[j], ext) == 0) {
                return r->category;
            }
        }
    }
    return "other";
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

int types_print(const type_map *tm, const char *rootpath,
                const types_options *o, FILE *out)
{
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32], nbuf[32];
    size_t i, shown;

    dd_fprintf(out, "%sUsage by extension in %s%s\n",
               dd_c(DD_C_BOLD), rootpath, dd_c(DD_C_RESET));
    if (tm->total_files == 0) {
        DD_FPUTS("No files found.\n", out);
        return 0;
    }
    dd_fprintf(out, "  %-10s  %8s  %12s  %7s  %-10s  %s\n",
               "EXT", "FILES", "SIZE", "%", "BAR", "CATEGORY");
    shown = (o->top > 0 && tm->len > (size_t)o->top) ? (size_t)o->top : tm->len;
    for (i = 0; i < shown; i++) {
        const type_entry *e = &tm->items[i];
        dd_strbuf bar;
        double pct = tm->total_bytes > 0
                         ? 100.0 * (double)e->bytes / (double)tm->total_bytes
                         : 0.0;
        dd_sb_init(&bar);
        dd_bar(&bar, pct / 100.0, 10);
        if (o->bytes_mode) {
            dd_comma(e->bytes, hsize, sizeof(hsize));
        } else {
            dd_human(e->bytes, sm, hsize, sizeof(hsize));
        }
        dd_fprintf(out, "  %-10s  %8s  %12s  %6.1f%%  %s%s%s  %s\n",
                   strcmp(e->ext, "(none)") == 0 ? "(none)" : e->ext,
                   dd_comma((uint64_t)e->count, nbuf, sizeof(nbuf)),
                   hsize, pct,
                   dd_c(DD_C_DIM), bar.data, dd_c(DD_C_RESET),
                   types_category(e->ext));
        dd_sb_free(&bar);
    }
    if (shown < tm->len) {
        dd_fprintf(out, "  ... %llu more extension types (use --top 0)\n",
                   (unsigned long long)(tm->len - shown));
    }
    dd_fprintf(out, "Total: %s file%s, %s across %llu extension type%s\n",
               dd_comma((uint64_t)tm->total_files, nbuf, sizeof(nbuf)),
               tm->total_files == 1 ? "" : "s",
               o->bytes_mode
                   ? dd_comma(tm->total_bytes, hsize, sizeof(hsize))
                   : dd_human(tm->total_bytes, sm, hsize, sizeof(hsize)),
               (unsigned long long)tm->len,
               tm->len == 1 ? "" : "s");
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

int types_json(const type_map *tm, const char *rootpath,
               const types_options *o, FILE *out)
{
    size_t i;
    dd_size_mode sm = o->si ? DD_SIZE_SI : DD_SIZE_BIN;
    char hsize[32];

    dd_fprintf(out, "{\"path\":");
    dd_json_quote(out, rootpath);
    dd_fprintf(out, ",\"total_bytes\":%llu,\"total_files\":%llu,\"types\":[",
               (unsigned long long)tm->total_bytes,
               (unsigned long long)tm->total_files);
    for (i = 0; i < tm->len; i++) {
        const type_entry *e = &tm->items[i];
        double pct = tm->total_bytes > 0
                         ? 100.0 * (double)e->bytes / (double)tm->total_bytes
                         : 0.0;
        dd_human(e->bytes, sm, hsize, sizeof(hsize));
        if (i > 0) {
            DD_FPUTC(',', out);
        }
        dd_fprintf(out, "{\"ext\":");
        dd_json_quote(out, e->ext);
        dd_fprintf(out, ",\"files\":%llu,\"bytes\":%llu,\"human\":\"%s\","
                        "\"pct\":%.2f,\"category\":\"%s\"}",
                   (unsigned long long)e->count,
                   (unsigned long long)e->bytes, hsize, pct,
                   types_category(e->ext));
    }
    DD_FPUTS("]}", out);
    return dd_out_ok(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* types command                                                       */
/* ------------------------------------------------------------------ */

enum { DD_TYPES_TOP = 1, DD_TYPES_SORT, DD_TYPES_BYTES, DD_TYPES_FOLLOW };

static const dd_option g_types_opts[] = {
    { "top", 'n', 1, DD_TYPES_TOP, "N",
      "show only the first N rows (0 = all, default 25)" },
    { "sort", 's', 1, DD_TYPES_SORT, "KEY",
      "sort rows by bytes|count|name (default: bytes)" },
    { "bytes", 'b', 0, DD_TYPES_BYTES, NULL,
      "print raw byte counts instead of human units" },
    { "follow", 'L', 0, DD_TYPES_FOLLOW, NULL,
      "follow symbolic links (default: never)" }
};

static void types_usage(FILE *out)
{
    dd_fprintf(out,
        "Usage: %s types [PATH] [options]\n\n"
        "Break disk usage down by file extension. Extensions are matched\n"
        "against the last dot of the file name and lowercased; files without\n"
        "one are grouped under \"(none)\". Each row also carries a category\n"
        "label (source, image, archive, ...).\n\n"
        "Options:\n", dd_progname);
    dd_print_options(out, g_types_opts, DD_NELTS(g_types_opts));
    DD_FPUTS("\nGlobal options (accepted anywhere): --json --si --color --no-color -h\n"
             "\nExamples:\n"
             "  diskdive types ~/src --top 15\n"
             "  diskdive types . --sort count\n"
             "\nExit codes: 0 ok · 1 runtime error · 2 usage error\n", out);
}

int types_cmd(int argc, char **argv)
{
    dd_globals *g = dd_globals_get();
    types_options o;
    scan_options so;
    type_map tm;
    scan_stats st;
    dd_args a;
    const char *path;
    char err[256];
    size_t i;
    int follow = 0, rc;

    memset(&o, 0, sizeof(o));
    o.top = 25;
    o.sort_key = DD_TYPESORT_BYTES_DESC;

    if (dd_args_parse(argc - 1, argv + 1, g_types_opts, DD_NELTS(g_types_opts),
                      g, &a, err, sizeof(err)) != 0) {
        dd_usage_error("types", err, g_types_opts, DD_NELTS(g_types_opts));
    }
    if (g->help) {
        types_usage(stdout);
        dd_args_free(&a);
        return DD_EXIT_OK;
    }
    if (a.npos > 1) {
        dd_usage_error("types", "too many arguments", g_types_opts, DD_NELTS(g_types_opts));
    }
    path = a.npos ? a.pos[0] : ".";

    for (i = 0; i < a.npairs; i++) {
        const dd_pair *p = &a.pairs[i];
        switch (p->id) {
        case DD_TYPES_TOP: {
            size_t v;
            if (dd_parse_count(p->val, &v, err, sizeof(err)) != 0) {
                dd_usage_error("types", err, g_types_opts, DD_NELTS(g_types_opts));
            }
            o.top = (int)v;
            break;
        }
        case DD_TYPES_SORT:
            if (strcmp(p->val, "bytes") == 0) {
                o.sort_key = DD_TYPESORT_BYTES_DESC;
            } else if (strcmp(p->val, "count") == 0) {
                o.sort_key = DD_TYPESORT_COUNT_DESC;
            } else if (strcmp(p->val, "name") == 0) {
                o.sort_key = DD_TYPESORT_NAME_ASC;
            } else {
                snprintf(err, sizeof(err),
                         "invalid --sort key '%s' (expected bytes|count|name)", p->val);
                dd_usage_error("types", err, g_types_opts, DD_NELTS(g_types_opts));
            }
            break;
        case DD_TYPES_BYTES:
            o.bytes_mode = 1;
            break;
        case DD_TYPES_FOLLOW:
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
    rc = types_collect(path, &so, &tm, &st, err, sizeof(err));
    if (rc != 0) {
        dd_error("%s", err);
        dd_args_free(&a);
        return DD_EXIT_RUNTIME;
    }
    types_sort(&tm, o.sort_key);
    rc = g->json ? types_json(&tm, path, &o, stdout)
                 : types_print(&tm, path, &o, stdout);
    types_free(&tm);
    dd_args_free(&a);
    if (rc != 0 || !dd_out_ok(stdout)) {
        if (!dd_out_ok(stdout)) {
            dd_error("write error");
        }
        return DD_EXIT_RUNTIME;
    }
    return DD_EXIT_OK;
}
