/*
 * tree.h — depth-limited directory tree with sizes (`tree` command).
 *
 * Implementation note: the tree reuses du_analyze() with keep_files=1
 * so every displayed directory shows its true aggregated size, exactly
 * like `du`. The walk is therefore always full-depth; --depth only
 * controls how deep the *printout* goes (frontier directories are
 * marked with "...").
 */
#ifndef DD_TREE_H
#define DD_TREE_H

#include <stdio.h>

#include "du.h"
#include "util.h"

/** Options for the `tree` subcommand. */
typedef struct {
    int depth;      /**< max printed levels; 0 = unlimited (default 3)  */
    int all;        /**< include hidden entries (default: excluded)     */
    int ascii;      /**< ASCII connectors (|-- `--) instead of Unicode  */
    int sort_key;   /**< DD_TREESORT_*                                  */
    int bytes_mode; /**< 1 = raw byte counts                            */
    int si;         /**< 1 = decimal units                              */
} tree_options;

/** Sort keys for tree entries. */
enum {
    DD_TREESORT_NAME_ASC = 0,   /**< directories first, then alpha        */
    DD_TREESORT_SIZE_DESC       /**< everything by aggregated size (desc) */
};

/**
 * Build the aggregation tree (full depth) and print it with
 * box-drawing connectors. Returns DD_EXIT_* code.
 */
int tree_run(const char *root, const scan_options *so, const tree_options *o,
             FILE *out);

/** Entry point of the `diskdive tree` subcommand (argv[0] = "tree"). */
int tree_cmd(int argc, char **argv);

#endif /* DD_TREE_H */
