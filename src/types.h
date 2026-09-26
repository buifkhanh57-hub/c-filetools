/*
 * types.h — disk usage breakdown by file extension (`types` command).
 *
 * Every file is classified by its lowercased extension (files without a
 * sensible extension are grouped under "(none)") and aggregated into
 * byte totals and counts. Extensions are additionally mapped to a
 * human-friendly category such as "source", "image" or "archive".
 */
#ifndef DD_TYPES_H
#define DD_TYPES_H

#include <stdio.h>

#include "scanner.h"
#include "util.h"

/** Maximum stored extension length (including NUL). */
#define DD_TYPE_EXT_MAX 24

/** One extension bucket. */
typedef struct {
    char ext[DD_TYPE_EXT_MAX]; /**< lowercased extension or "(none)" */
    uint64_t bytes;            /**< sum of st_size                   */
    size_t count;              /**< number of files                  */
} type_entry;

/** Aggregated extension map. */
typedef struct {
    type_entry *items;
    size_t len;
    size_t cap;
    uint64_t total_bytes; /**< sum over all files                     */
    size_t total_files;   /**< number of files classified             */
} type_map;

/** Options for the `types` subcommand. */
typedef struct {
    int top;        /**< show only the first N rows; 0 = all (default 25) */
    int sort_key;   /**< DD_TYPESORT_*                                    */
    int bytes_mode; /**< 1 = raw byte counts                              */
    int si;         /**< 1 = decimal units                                */
} types_options;

/** Sort keys. */
enum {
    DD_TYPESORT_BYTES_DESC = 0,
    DD_TYPESORT_COUNT_DESC,
    DD_TYPESORT_NAME_ASC
};

/** Walk @p root and aggregate extensions into @p tm. */
int types_collect(const char *root, const scan_options *so, type_map *tm,
                  scan_stats *st, char *err, size_t errn);

/** Deterministic sort of the bucket array by @p key. */
void types_sort(type_map *tm, int key);

/** Pretty table with percentage bars and categories. */
int types_print(const type_map *tm, const char *rootpath,
                const types_options *o, FILE *out);

/** JSON emitter (all buckets, sorted). */
int types_json(const type_map *tm, const char *rootpath,
               const types_options *o, FILE *out);

void types_free(type_map *tm);

/** Category label for an extension ("py" → "source"). */
const char *types_category(const char *ext);

/** Entry point of the `diskdive types` subcommand (argv[0] = "types"). */
int types_cmd(int argc, char **argv);

#endif /* DD_TYPES_H */
