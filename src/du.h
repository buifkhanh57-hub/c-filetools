/*
 * du.h — post-order directory size aggregation and the `du` subcommand.
 *
 * du_analyze() builds an in-memory directory tree from a scanner walk.
 * Every node aggregates its direct children, so printing, sorting and
 * percentage bars are pure memory operations afterwards.
 */
#ifndef DD_DU_H
#define DD_DU_H

#include <stdio.h>

#include "scanner.h"
#include "util.h"

/** Leaf payload kept per directory when keep_files is requested. */
typedef struct {
    char *name;      /**< owned file name (basename) */
    uint64_t size;   /**< st_size                    */
} du_file;

/** One directory node. Root has depth 0 and parent == NULL. */
typedef struct du_node {
    char *name;             /**< path segment; "/" for the root "/"    */
    uint64_t own_bytes;     /**< bytes of files directly inside        */
    uint64_t own_blocks;    /**< on-disk bytes of files directly inside*/
    uint64_t total_bytes;   /**< own + all descendants                 */
    uint64_t total_blocks;  /**< own + all descendants (on-disk)       */
    size_t file_count;      /**< files in the whole subtree            */
    size_t dir_count;       /**< directories in the whole subtree      */
    struct du_node **children;
    size_t nchildren;
    size_t childcap;
    du_file *files;         /**< only filled when keep_files is set    */
    size_t nfiles;
    size_t filecap;
    struct du_node *parent;
    int depth;
} du_node;

/** Options for analysis and printing. */
typedef struct {
    int depth;      /**< max printed levels; 0 = unlimited (walk is always full) */
    int sort_key;   /**< DD_DUSORT_*                                             */
    int top;        /**< print only the first N rows; 0 = all (default 40)       */
    int apparent;   /**< rank by st_size instead of on-disk blocks               */
    int bytes_mode; /**< 1 = print raw byte counts instead of human units        */
    int si;         /**< 1 = decimal units (KB/MB) instead of binary (KiB/MiB)   */
    int keep_files; /**< remember per-directory file entries (used by tree)      */
} du_options;

/** Sort keys for du rows. */
enum {
    DD_DUSORT_SIZE_DESC = 0,
    DD_DUSORT_NAME_ASC,
    DD_DUSORT_FILES_DESC
};

/**
 * Walk @p root and build the aggregation tree.
 * Returns 0 and sets *out on success; -1 when root is inaccessible.
 */
int du_analyze(const char *root, const du_options *o, const scan_options *so,
               du_node **out, scan_stats *st, char *err, size_t errn);

/**
 * Print the classic diskdive du table for the children of @p root,
 * filtered by o->depth and truncated to o->top rows.
 */
int du_print(const du_node *root, const du_options *o, const char *rootpath,
             const scan_stats *st, FILE *out);

/** JSON variant of du_print (object with a recursive "children" array). */
int du_json(const du_node *root, const du_options *o, const char *rootpath,
            const scan_stats *st, FILE *out);

/** Metric-aware size of a node (apparent bytes or on-disk blocks). */
uint64_t du_node_size(const du_node *n, int apparent);

/**
 * Snapshot of @p parent's direct children, sorted by @p key
 * (DD_DUSORT_*). The caller owns the returned array (free()) but not
 * the nodes inside it. Used by report for the "largest dirs" section.
 */
void du_sort_children(const du_node *parent, int key, int apparent,
                      const du_node ***rows, size_t *nrows);

/** Full path of @p n given the scan root path; appended to @p sb. */
void du_node_path(const du_node *n, const char *rootpath, dd_strbuf *sb);

/** Deep-free the tree. */
void du_free(du_node *root);

/** Entry point of the `diskdive du` subcommand (argv[0] = "du"). */
int du_cmd(int argc, char **argv);

#endif /* DD_DU_H */
