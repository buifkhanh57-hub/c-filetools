/*
 * hash.c — FNV-1a/64 streaming hash and chunked file reader.
 */
#include "hash.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void dd_hash_init(dd_hash_ctx *c)
{
    c->state = DD_FNV_OFFSET_BASIS;
    c->len = 0;
}

void dd_hash_update(dd_hash_ctx *c, const void *data, size_t n)
{
    const unsigned char *p = data;
    uint64_t h = c->state;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= DD_FNV_PRIME;
    }
    c->state = h;
    c->len += (uint64_t)n;
}

uint64_t dd_hash_final(dd_hash_ctx *c)
{
    return c->state;
}

uint64_t dd_hash_buf(const void *data, size_t n)
{
    dd_hash_ctx c;
    dd_hash_init(&c);
    dd_hash_update(&c, data, n);
    return dd_hash_final(&c);
}

int dd_hash_file(const char *path, uint64_t limit, uint64_t *out,
                 char *err, size_t errn)
{
    int fd;
    char *buf;
    uint64_t digest;
    dd_hash_ctx c;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errn, "'%s': %s", path, strerror(errno));
        return -1;
    }

    buf = dd_xmalloc(DD_HASH_CHUNK);
    dd_hash_init(&c);
    digest = c.state;

    for (;;) {
        size_t want = DD_HASH_CHUNK;
        if (limit > 0) {
            uint64_t left = limit - c.len;
            if (left == 0) {
                break;
            }
            if (left < (uint64_t)want) {
                want = (size_t)left;
            }
        }
        ssize_t got = read(fd, buf, want);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(err, errn, "'%s': %s", path, strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        if (got == 0) {
            break;
        }
        dd_hash_update(&c, buf, (size_t)got);
    }

    digest = dd_hash_final(&c);
    free(buf);
    if (close(fd) != 0) {
        snprintf(err, errn, "'%s': close: %s", path, strerror(errno));
        return -1;
    }
    *out = digest;
    return 0;
}
