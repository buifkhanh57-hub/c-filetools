/*
 * clean.h — cache/tmp candidate discovery and (optional) deletion.
 *
 * Safety model:
 *  - every run is a DRY RUN unless --delete is given;
 *  --delete without --yes asks for a typed confirmation when stdin is
 *     interactive, and refuses to run non-interactively;
 *  - the scan root itself is never a candidate, and "/" is rejected
 *     for deletion entirely;
 *  - symlinks are unlinked, never followed, when deleting.
 */
#ifndef DD_CLEAN_H
#define DD_CLEAN_H

#include <stdio.h>

#include "scanner.h"
#include "util.h"

/** One deletion candidate. */
typedef struct {
    char *path;       /**< owned full path                            */
    uint64_t bytes;   /**< reclaimable bytes (recursive for dirs)     */
    int is_dir;       /**< 1 for cache directories                    */
    char *reason;     /**< owned human explanation ("matched *.log")  */
    int age_days;     /**< file age in days, -1 for directories       */
} clean_item;

typedef struct {
    clean_item *items;
    size_t len;
    size_t cap;
    uint64_t total_bytes;
} clean_list;

/** Rules describing what counts as junk. */
typedef struct {
    dd_strlist file_globs; /**< fnmatch patterns for files ("*.tmp")   */
    dd_strlist dir_names;  /**< literal directory names to prune       */
    int older_than_days;   /**< files must be at least this old (0=any)*/
    int follow;            /**< follow symlinked dirs while scanning   */
} clean_options;

/** Fill clean_options with the built-in rules (see README.md). */
void clean_options_defaults(clean_options *o);

void clean_options_free(clean_options *o);

/**
 * Walk @p root and collect candidates. Cache directories are measured
 * (recursive size) but NOT descended into, so nested cache dirs only
 * appear once. Returns 0 on success, -1 when @p root is inaccessible.
 */
int clean_collect(const char *root, const clean_options *o, clean_list *out,
                  scan_stats *st, char *err, size_t errn);

/** Print the candidate table. @p is_dryrun adds the DRY RUN banner. */
int clean_print(const clean_list *list, const char *rootpath,
                int bytes_mode, int si, int is_dryrun, FILE *out);

/**
 * Delete every candidate. When @p assume_yes is 0 an interactive
 * confirmation is required; refusing or EOF aborts with exit code 1.
 * Returns DD_EXIT_OK or DD_EXIT_RUNTIME.
 */
int clean_execute(const char *rootpath, const clean_list *list,
                  int assume_yes, size_t *deleted, uint64_t *freed,
                  size_t *errors, FILE *in, FILE *out);

void clean_list_free(clean_list *list);

/** Entry point of the `diskdive clean` subcommand (argv[0] = "clean"). */
int clean_cmd(int argc, char **argv);

#endif /* DD_CLEAN_H */
