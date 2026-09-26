/*
 * top.h — "N largest files" via a bounded streaming min-heap.
 *
 * The heap keeps at most N entries at any time, so memory stays O(N)
 * no matter how many files the walk encounters. Files smaller than the
 * current heap minimum are dropped without touching the heap.
 */
#ifndef DD_TOP_H
#define DD_TOP_H

#include <stdio.h>

#include "scanner.h"
#include "util.h"

/** One ranked entry. */
typedef struct {
    char *path;    /**< owned full path                     */
    uint64_t size; /**< apparent size (st_size)             */
    int64_t mtime; /**< last modification time              */
} top_entry;

/** Bounded min-heap ordered by size. */
typedef struct {
    top_entry *e;
    size_t len;   /**< entries currently stored            */
    size_t cap;   /**< allocated slots                     */
    size_t n;     /**< heap bound (max entries to keep)    */
} top_heap;

void top_heap_init(top_heap *h, size_t n);

/**
 * Offer one file to the heap. Returns 1 if it was kept (heap not full
 * or larger than the current minimum), 0 if it was dropped. Ownership
 * of @p path is NOT taken — the heap stores its own copy.
 */
int top_heap_offer(top_heap *h, const char *path, uint64_t size, int64_t mtime);

/** Sort the heap contents by size (desc), path (asc) for output. */
void top_heap_sort_desc(top_heap *h);

void top_heap_free(top_heap *h);

/** Options for the `top` subcommand. */
typedef struct {
    size_t n;         /**< how many files to report (>= 1)     */
    uint64_t min_size;/**< skip smaller files (suffix syntax)  */
    int bytes_mode;   /**< 1 = raw byte counts                 */
    int si;           /**< 1 = decimal units                   */
} top_options;

/**
 * Walk @p root and feed every regular file into @p h.
 * Uses a streaming visitor — never materializes the file list.
 */
int top_collect(const char *root, const scan_options *so, const top_options *o,
                top_heap *h, scan_stats *st, char *err, size_t errn);

/** Full `top` command: collect, sort and print the ranking table. */
int top_run(const char *root, const scan_options *so, const top_options *o,
            FILE *out);

/** JSON emitter for a finished heap (expects top_heap_sort_desc). */
int top_json(const top_heap *h, const char *rootpath, const scan_stats *st,
             const top_options *o, FILE *out);

/** Entry point of the `diskdive top` subcommand (argv[0] = "top"). */
int top_cmd(int argc, char **argv);

#endif /* DD_TOP_H */
