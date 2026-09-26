/*
 * scanner.h — recursive directory traversal for diskdive.
 *
 * The scanner is the single place where diskdive touches the filesystem:
 * every subcommand is a consumer of scan_walk() or scan_collect_files().
 *
 * Guarantees:
 *  - paths are built in a dynamically grown buffer (no PATH_MAX limits);
 *  - unreadable entries (EACCES, ELOOP, ...) are counted in
 *    scan_stats.errors and reported via the on_error callback, the walk
 *    continues;
 *  - symbolic links are never followed unless follow_symlinks is set,
 *    and directory loops are detected via (dev, ino) bookkeeping;
 *  - entries are visited in byte-sorted order per directory, so output
 *    is deterministic.
 */
#ifndef DD_SCANNER_H
#define DD_SCANNER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "util.h"

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

/** Aggregated counters filled by every walk. */
typedef struct {
    uint64_t files;        /**< regular files visited                 */
    uint64_t dirs;         /**< directories visited (excluding root)  */
    uint64_t symlinks;     /**< symlinks seen (followed or not)       */
    uint64_t special;      /**< fifos, sockets, device nodes          */
    uint64_t errors;       /**< lstat/open/read failures (e.g. EACCES)*/
    uint64_t total_bytes;  /**< sum of st_size over regular files     */
    uint64_t total_blocks; /**< sum of st_blocks * 512 (on-disk bytes)*/
} scan_stats;

void scan_stats_init(scan_stats *st);

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

/** Traversal policy. Build with scan_options_defaults() and override. */
typedef struct {
    int follow_symlinks;   /**< descend into symlinked directories   */
    int include_hidden;    /**< visit dot-files and dot-directories  */
    int stay_on_fs;        /**< do not cross mount points            */
    const dd_strlist *prune_dirs; /**< literal dir names to skip (may be NULL) */
} scan_options;

void scan_options_defaults(scan_options *o);

/* ------------------------------------------------------------------ */
/* Visitor interface                                                   */
/* ------------------------------------------------------------------ */

/**
 * Callback set used by scan_walk(). Any callback may be NULL.
 *
 * on_dir_pre() is special: returning non-zero skips the whole subtree;
 * on_dir_post() is then not invoked for that directory. The root
 * directory itself never triggers on_dir_pre/on_dir_pre, but it does
 * trigger on_dir_post when the walk over it completes.
 */
typedef struct scan_visitor {
    void *ud; /**< opaque context handed back to every callback       */

    void (*on_file)(void *ud, const char *path, const struct stat *st, int depth);
    int  (*on_dir_pre)(void *ud, const char *path, const struct stat *st, int depth);
    void (*on_dir_post)(void *ud, const char *path, int depth);
    void (*on_symlink)(void *ud, const char *path, const struct stat *st, int depth);
    void (*on_error)(void *ud, const char *path, int eno);
} scan_visitor;

/* ------------------------------------------------------------------ */
/* Core walk                                                           */
/* ------------------------------------------------------------------ */

/**
 * Walk @p root (a directory, a file, or a symlink). Returns 0 on
 * success; -1 when @p root itself cannot be accessed, with @p err
 * filled. Individual entry failures do not fail the walk.
 */
int scan_walk(const char *root, const scan_options *o, const scan_visitor *v,
              scan_stats *st, char *err, size_t errn);

/* ------------------------------------------------------------------ */
/* Flat file list (used by dups)                                       */
/* ------------------------------------------------------------------ */

/** One regular file discovered during a walk. */
typedef struct {
    char    *path;   /**< owned copy of the full path            */
    uint64_t size;   /**< st_size (apparent size)                */
    uint64_t blocks; /**< st_blocks * 512 (on-disk size)         */
    int64_t  mtime;  /**< st_mtime as a signed 64-bit value      */
    dev_t    dev;    /**< device (hardlink detection)            */
    ino_t    ino;    /**< inode  (hardlink detection)            */
} file_entry;

typedef struct {
    file_entry *items;
    size_t len;
    size_t cap;
} file_list;

/** Walk @p root and append every regular file to @p fl. */
int scan_collect_files(const char *root, const scan_options *o, file_list *fl,
                       scan_stats *st, char *err, size_t errn);

void file_list_free(file_list *fl);

/** Sort keys for file_list_sort(). */
enum {
    DD_FLSORT_SIZE_DESC = 0,
    DD_FLSORT_SIZE_ASC,
    DD_FLSORT_MTIME_ASC,
    DD_FLSORT_MTIME_DESC,
    DD_FLSORT_PATH_ASC
};

/** Stable, deterministic sort of @p fl by @p key. */
void file_list_sort(file_list *fl, int key);

#endif /* DD_SCANNER_H */
