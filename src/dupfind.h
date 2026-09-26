/*
 * dupfind.h — duplicate file detection pipeline.
 *
 * Three-stage pipeline designed to touch as few bytes as possible:
 *
 *   1. size prefilter   — only files sharing a size can be duplicates;
 *   2. FNV-1a/64 hash   — files are hashed in 64 KiB chunks; with
 *                         --quick only the first 64 KiB are hashed;
 *   3. byte-exact verify — candidate groups are compared with memcmp
 *                         on re-read, so hash collisions are harmless.
 *
 * Hard links to the same inode are detected and reported separately:
 * they are not duplicates on disk, just the same inode seen twice.
 */
#ifndef DD_DUPFIND_H
#define DD_DUPFIND_H

#include <stdio.h>

#include "scanner.h"
#include "util.h"

/** One confirmed duplicate group. */
typedef struct {
    uint64_t size;    /**< size of every file in the group            */
    size_t nfiles;    /**< number of identical files (>= 2)           */
    char **paths;     /**< owned paths, scan order                    */
} dup_group;

/** Result of dups_find(). */
typedef struct {
    dup_group *groups;
    size_t len;
    size_t cap;
    size_t redundant_files;  /**< sum over groups of (nfiles - 1)        */
    uint64_t wasted_bytes;   /**< sum over groups of (nfiles - 1) * size */
    size_t hardlink_skips;   /**< entries skipped because of shared inodes */
    uint64_t scanned_bytes;  /**< apparent bytes scanned (for % of total)  */
} dup_result;

/** Options for dups_find(). */
typedef struct {
    uint64_t min_size; /**< files below this size are ignored (0 = all) */
    int quick;         /**< hash only the first DD_HASH_CHUNK bytes     */
} dup_options;

/**
 * Run the full pipeline on @p root. Returns 0 on success (including
 * "no duplicates"); -1 when @p root cannot be accessed. Individual
 * unreadable files are warned about and skipped.
 */
int dups_find(const char *root, const scan_options *so, const dup_options *o,
              dup_result *res, scan_stats *st, char *err, size_t errn);

/** Human-readable group report. @p summary prints only the footer. */
int dups_print(const dup_result *res, const char *rootpath, int summary,
               int bytes_mode, int si, FILE *out);

/** JSON emitter: groups with their member paths. */
int dups_json(const dup_result *res, const char *rootpath,
              const dup_options *o, FILE *out);

/** Release all memory owned by @p res. */
void dups_free(dup_result *res);

/** Entry point of the `diskdive dups` subcommand (argv[0] = "dups"). */
int dups_cmd(int argc, char **argv);

#endif /* DD_DUPFIND_H */
