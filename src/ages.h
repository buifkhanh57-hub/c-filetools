/*
 * ages.h — file age distribution by modification time (`ages` command).
 *
 * Files are grouped into five fixed buckets relative to the current
 * time, giving a quick "how stale is this tree" answer. The newest and
 * oldest files are additionally highlighted.
 */
#ifndef DD_AGES_H
#define DD_AGES_H

#include <stdio.h>

#include "scanner.h"
#include "util.h"

/** Number of fixed age buckets. */
#define DD_AGE_BUCKETS 5

/* Bucket boundaries in seconds (upper bounds, exclusive). */
#define DD_AGE_1D  (1LL * 24 * 60 * 60)
#define DD_AGE_7D  (7LL * 24 * 60 * 60)
#define DD_AGE_30D (30LL * 24 * 60 * 60)
#define DD_AGE_1Y  (365LL * 24 * 60 * 60)

/** Per-bucket aggregation. */
typedef struct {
    uint64_t files; /**< files in this bucket        */
    uint64_t bytes; /**< sum of st_size in the bucket */
} age_bucket;

/** Full result of ages_collect(). */
typedef struct {
    age_bucket b[DD_AGE_BUCKETS];
    int64_t now;              /**< reference time used for bucketing  */
    char *newest_path;        /**< owned, NULL when no files          */
    int64_t newest_mtime;
    char *oldest_path;        /**< owned, NULL when no files          */
    int64_t oldest_mtime;
} age_report;

/** Options for the `ages` subcommand. */
typedef struct {
    int bytes_mode; /**< 1 = raw byte counts              */
    int si;         /**< 1 = decimal units                */
} ages_options;

/** Human-readable bucket name ("<1 day", "1-7 days", ...). */
const char *age_bucket_name(int bucket);

/** Walk @p root and bucket every regular file by mtime. */
int ages_collect(const char *root, const scan_options *so, age_report *ar,
                 scan_stats *st, char *err, size_t errn);

/** Table printout with newest/oldest highlights. */
int ages_print(const age_report *ar, const char *rootpath,
               const ages_options *o, FILE *out);

/** JSON emitter (buckets + newest/oldest or null). */
int ages_json(const age_report *ar, const char *rootpath,
              const ages_options *o, FILE *out);

void ages_free(age_report *ar);

/** Entry point of the `diskdive ages` subcommand (argv[0] = "ages"). */
int ages_cmd(int argc, char **argv);

#endif /* DD_AGES_H */
