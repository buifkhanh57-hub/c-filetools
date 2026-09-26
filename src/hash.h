/*
 * hash.h — streaming FNV-1a 64-bit hashing and chunked file hashing.
 *
 * FNV-1a is not cryptographic; it is chosen deliberately as a cheap
 * prefilter for duplicate detection. Every hash match produced by
 * diskdive is later confirmed with a byte-exact comparison (see
 * dupfind.c), so collisions can never cause false positives — they only
 * cost an extra comparison.
 */
#ifndef DD_HASH_H
#define DD_HASH_H

#include <stddef.h>
#include <stdint.h>

/** Streaming FNV-1a/64 context. */
typedef struct {
    uint64_t state;   /**< current hash value                       */
    uint64_t len;     /**< number of bytes fed so far               */
} dd_hash_ctx;

#define DD_FNV_OFFSET_BASIS 14695981039346656037ULL
#define DD_FNV_PRIME        1099511628211ULL

/** I/O chunk used when hashing or comparing files (64 KiB). */
#define DD_HASH_CHUNK (64u * 1024u)

/** Reset @p c to the FNV-1a offset basis. */
void dd_hash_init(dd_hash_ctx *c);

/** Feed @p n bytes at @p data into the hash. */
void dd_hash_update(dd_hash_ctx *c, const void *data, size_t n);

/** Finish the stream and return the 64-bit digest. */
uint64_t dd_hash_final(dd_hash_ctx *c);

/** One-shot helper for in-memory buffers. */
uint64_t dd_hash_buf(const void *data, size_t n);

/**
 * Hash a file's content in DD_HASH_CHUNK-sized reads.
 *
 * @param path  file to hash (opened read-only, never written)
 * @param limit stop after @p limit bytes; 0 hashes the whole file
 *              (the "quick" mode used by `dups --quick` and `report`)
 * @param out   receives the digest on success
 * @param err   buffer receiving "path: reason" on failure
 *
 * Returns 0 on success, -1 on failure (stat/open/read/close errors).
 */
int dd_hash_file(const char *path, uint64_t limit, uint64_t *out,
                 char *err, size_t errn);

#endif /* DD_HASH_H */
