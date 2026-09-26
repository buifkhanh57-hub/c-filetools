/*
 * scanner.c — recursive readdir() walker with dynamic path buffers,
 * symlink policies, mount-point awareness and per-error tolerance.
 */
#include "hash.h"
#include "scanner.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Loop detection (dev, ino) table — only active when following links  */
/* ------------------------------------------------------------------ */

typedef struct {
    dev_t dev;
    ino_t ino;
} vis_key;

typedef struct {
    vis_key *keys;
    unsigned char *used;
    size_t cap;   /* power of two */
    size_t count;
} vis_table;

static uint64_t vis_hash(dev_t dev, ino_t ino)
{
    dd_hash_ctx c;
    dd_hash_init(&c);
    dd_hash_update(&c, &dev, sizeof(dev));
    dd_hash_update(&c, &ino, sizeof(ino));
    return dd_hash_final(&c);
}

static void vis_init(vis_table *t, size_t cap0)
{
    size_t cap = 64;
    while (cap < cap0) {
        cap *= 2;
    }
    t->keys = dd_xcalloc(cap, sizeof(*t->keys));
    t->used = dd_xcalloc(cap, 1);
    t->cap = cap;
    t->count = 0;
}

static void vis_free(vis_table *t)
{
    free(t->keys);
    free(t->used);
    t->keys = NULL;
    t->used = NULL;
    t->cap = 0;
    t->count = 0;
}

static size_t vis_probe(const vis_table *t, dev_t dev, ino_t ino)
{
    size_t mask = t->cap - 1;
    size_t idx = (size_t)(vis_hash(dev, ino) & (uint64_t)mask);
    while (t->used[idx] && !(t->keys[idx].dev == dev && t->keys[idx].ino == ino)) {
        idx = (idx + 1) & mask;
    }
    return idx;
}

static int vis_has(const vis_table *t, dev_t dev, ino_t ino)
{
    if (t->cap == 0) {
        return 0;
    }
    return t->used[vis_probe(t, dev, ino)] != 0;
}

static void vis_add(vis_table *t, dev_t dev, ino_t ino)
{
    size_t idx;
    if (t->cap == 0) {
        vis_init(t, 64);
    }
    if (t->count * 10 >= t->cap * 7) { /* grow at 70% load */
        vis_table nt;
        size_t i;
        vis_init(&nt, t->cap * 2);
        for (i = 0; i < t->cap; i++) {
            if (t->used[i]) {
                vis_add(&nt, t->keys[i].dev, t->keys[i].ino);
            }
        }
        vis_free(t);
        *t = nt;
    }
    idx = vis_probe(t, dev, ino);
    if (!t->used[idx]) {
        t->used[idx] = 1;
        t->keys[idx].dev = dev;
        t->keys[idx].ino = ino;
        t->count++;
    }
}

/* ------------------------------------------------------------------ */
/* Walk context                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const scan_options *o;
    const scan_visitor *v;
    scan_stats *st;
    dd_strbuf path;     /* current directory being scanned          */
    dev_t root_dev;     /* device of the root (stay_on_fs)          */
    vis_table visited;  /* valid only when follow_symlinks          */
} walk_ctx;

static void ctx_error(walk_ctx *c, const char *path, int eno)
{
    c->st->errors++;
    if (c->v->on_error) {
        c->v->on_error(c->v->ud, path, eno);
    }
}

static void notify_file(walk_ctx *c, const char *path, const struct stat *st, int depth)
{
    c->st->files++;
    c->st->total_bytes += (uint64_t)st->st_size;
    c->st->total_blocks += (uint64_t)st->st_blocks * 512u;
    if (c->v->on_file) {
        c->v->on_file(c->v->ud, path, st, depth);
    }
}

static int name_list_cmp(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

/* Build "dir/name" (or "dir/name" when dir is "/") in ctx->path. */
static void join_entry(walk_ctx *c, size_t dlen, const char *name)
{
    dd_sb_truncate(&c->path, dlen);
    if (c->path.len == 0 || c->path.data[c->path.len - 1] != '/') {
        dd_sb_appendch(&c->path, '/');
    }
    dd_sb_append(&c->path, name);
}

static int walk_dir(walk_ctx *c, int depth)
{
    size_t dlen = c->path.len;
    const char *dirpath = c->path.data;
    DIR *d;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    size_t i;

    d = opendir(dirpath);
    if (!d) {
        ctx_error(c, dirpath, errno);
        return 0;
    }

    errno = 0;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = dd_xrealloc(names, cap * sizeof(*names));
        }
        names[n++] = dd_xstrdup(nm);
    }
    if (errno != 0) {
        ctx_error(c, dirpath, errno);
        errno = 0;
    }
    closedir(d);

    qsort(names, n, sizeof(*names), name_list_cmp);

    for (i = 0; i < n; i++) {
        char *name = names[i];
        struct stat st;

        if (!c->o->include_hidden && dd_name_is_hidden(name)) {
            free(name);
            continue;
        }
        join_entry(c, dlen, name);

        if (lstat(c->path.data, &st) != 0) {
            ctx_error(c, c->path.data, errno);
            free(name);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (c->o->prune_dirs && dd_sl_contains(c->o->prune_dirs, name)) {
                free(name);
                continue;
            }
            if (c->o->stay_on_fs && st.st_dev != c->root_dev) {
                free(name);
                continue;
            }
            if (c->o->follow_symlinks) {
                if (vis_has(&c->visited, st.st_dev, st.st_ino)) {
                    free(name);
                    continue;
                }
                vis_add(&c->visited, st.st_dev, st.st_ino);
            }
            c->st->dirs++;
            if (c->v->on_dir_pre && c->v->on_dir_pre(c->v->ud, c->path.data, &st, depth)) {
                free(name);
                continue; /* subtree skipped; no post callback */
            }
            walk_dir(c, depth + 1);
            dd_sb_truncate(&c->path, dlen);
        } else if (S_ISLNK(st.st_mode)) {
            c->st->symlinks++;
            if (c->o->follow_symlinks) {
                struct stat tst;
                if (stat(c->path.data, &tst) == 0 && S_ISDIR(tst.st_mode)) {
                    if (!vis_has(&c->visited, tst.st_dev, tst.st_ino)) {
                        vis_add(&c->visited, tst.st_dev, tst.st_ino);
                        if (c->v->on_symlink) {
                            c->v->on_symlink(c->v->ud, c->path.data, &st, depth);
                        }
                        c->st->dirs++;
                        if (c->v->on_dir_pre &&
                            !c->v->on_dir_pre(c->v->ud, c->path.data, &tst, depth)) {
                            walk_dir(c, depth + 1);
                            dd_sb_truncate(&c->path, dlen);
                        }
                    }
                } else if (c->v->on_symlink) {
                    c->v->on_symlink(c->v->ud, c->path.data, &st, depth);
                }
            } else if (c->v->on_symlink) {
                c->v->on_symlink(c->v->ud, c->path.data, &st, depth);
            }
        } else if (S_ISREG(st.st_mode)) {
            notify_file(c, c->path.data, &st, depth);
        } else {
            c->st->special++;
        }
        free(name);
    }

    free(names);

    dd_sb_truncate(&c->path, dlen);
    if (c->v->on_dir_post) {
        c->v->on_dir_post(c->v->ud, c->path.data, depth);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

void scan_stats_init(scan_stats *st)
{
    memset(st, 0, sizeof(*st));
}

void scan_options_defaults(scan_options *o)
{
    o->follow_symlinks = 0;
    o->include_hidden = 1;
    o->stay_on_fs = 0;
    o->prune_dirs = NULL;
}

static int scan_stat_root(const char *root, int follow, struct stat *st,
                          char *err, size_t errn)
{
    if (lstat(root, st) != 0) {
        snprintf(err, errn, "cannot access '%s': %s", root, strerror(errno));
        return -1;
    }
    if (follow && S_ISLNK(st->st_mode)) {
        if (stat(root, st) != 0) {
            snprintf(err, errn, "cannot resolve '%s': %s", root, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int scan_walk(const char *root, const scan_options *o, const scan_visitor *v,
              scan_stats *st, char *err, size_t errn)
{
    walk_ctx c;
    struct stat st0;
    size_t rlen;
    int rc = 0;

    if (!root || !*root) {
        snprintf(err, errn, "empty path");
        return -1;
    }
    if (scan_stat_root(root, o->follow_symlinks, &st0, err, errn) != 0) {
        return -1;
    }

    memset(&c, 0, sizeof(c));
    c.o = o;
    c.v = v;
    c.st = st;
    dd_sb_init(&c.path);
    if (o->follow_symlinks) {
        vis_init(&c.visited, 64);
    }

    /* normalize root: collapse trailing slashes (keep "/" itself) */
    dd_sb_append(&c.path, root);
    rlen = c.path.len;
    while (rlen > 1 && c.path.data[rlen - 1] == '/') {
        rlen--;
    }
    dd_sb_truncate(&c.path, rlen);
    c.root_dev = st0.st_dev;

    if (S_ISDIR(st0.st_mode)) {
        if (o->follow_symlinks) {
            vis_add(&c.visited, st0.st_dev, st0.st_ino);
        }
        walk_dir(&c, 0);
    } else if (S_ISREG(st0.st_mode)) {
        notify_file(&c, c.path.data, &st0, 0);
    } else if (S_ISLNK(st0.st_mode)) {
        st->symlinks++;
        if (v->on_symlink) {
            v->on_symlink(v->ud, c.path.data, &st0, 0);
        }
    } else {
        st->special++;
    }

    dd_sb_free(&c.path);
    vis_free(&c.visited);
    if (rc != 0) {
        return rc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Flat file list                                                      */
/* ------------------------------------------------------------------ */

static void fl_push(file_list *fl, const char *path, const struct stat *st)
{
    file_entry *e;
    if (fl->len == fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 128;
        fl->items = dd_xrealloc(fl->items, fl->cap * sizeof(*fl->items));
    }
    e = &fl->items[fl->len++];
    e->path = dd_xstrdup(path);
    e->size = (uint64_t)st->st_size;
    e->blocks = (uint64_t)st->st_blocks * 512u;
    e->mtime = (int64_t)st->st_mtime;
    e->dev = st->st_dev;
    e->ino = st->st_ino;
}

typedef struct {
    file_list *fl;
} collect_ctx;

static void collect_on_file(void *ud, const char *path, const struct stat *st, int depth)
{
    collect_ctx *cc = ud;
    (void)depth;
    fl_push(cc->fl, path, st);
}

int scan_collect_files(const char *root, const scan_options *o, file_list *fl,
                       scan_stats *st, char *err, size_t errn)
{
    scan_visitor v;
    collect_ctx cc;

    memset(fl, 0, sizeof(*fl));
    cc.fl = fl;
    memset(&v, 0, sizeof(v));
    v.ud = &cc;
    v.on_file = collect_on_file;
    return scan_walk(root, o, &v, st, err, errn);
}

void file_list_free(file_list *fl)
{
    size_t i;
    if (!fl) {
        return;
    }
    for (i = 0; i < fl->len; i++) {
        free(fl->items[i].path);
    }
    free(fl->items);
    fl->items = NULL;
    fl->len = 0;
    fl->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

static int g_flkey = DD_FLSORT_SIZE_DESC;

static int fl_entry_cmp(const void *a, const void *b)
{
    const file_entry *pa = a;
    const file_entry *pb = b;
    int c;

    switch (g_flkey) {
    case DD_FLSORT_SIZE_ASC:
        if (pa->size != pb->size) {
            return pa->size < pb->size ? -1 : 1;
        }
        break;
    case DD_FLSORT_MTIME_ASC:
        if (pa->mtime != pb->mtime) {
            return pa->mtime < pb->mtime ? -1 : 1;
        }
        break;
    case DD_FLSORT_MTIME_DESC:
        if (pa->mtime != pb->mtime) {
            return pa->mtime > pb->mtime ? -1 : 1;
        }
        break;
    case DD_FLSORT_PATH_ASC:
        return strcmp(pa->path, pb->path);
    default: /* DD_FLSORT_SIZE_DESC */
        if (pa->size != pb->size) {
            return pa->size > pb->size ? -1 : 1;
        }
        break;
    }
    c = strcmp(pa->path, pb->path);
    return c;
}

void file_list_sort(file_list *fl, int key)
{
    g_flkey = key;
    qsort(fl->items, fl->len, sizeof(*fl->items), fl_entry_cmp);
}
