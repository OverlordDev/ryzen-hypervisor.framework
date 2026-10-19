/*
 * RHVRing.h - byte ring buffer with absolute cursors.
 *
 * Header-only, no kernel dependencies, so it is unit-tested on the host
 * (tests/host_test.c) and used as-is by the kext log (kext/Source/Log.cpp).
 * No locking here: the caller serialises access.
 *
 * Writers never block: when the ring is full the oldest bytes are
 * overwritten. Readers keep an absolute cursor and learn how many bytes
 * they missed.
 */
#ifndef RYZENHV_RING_H
#define RYZENHV_RING_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RHVRing {
    char    *buf;
    uint32_t size;
    uint64_t head;   /* total bytes ever written */
} RHVRing;

static inline void rhv_ring_init(RHVRing *r, char *buf, uint32_t size)
{
    r->buf  = buf;
    r->size = size;
    r->head = 0;
}

static inline void rhv_ring_write(RHVRing *r, const char *data, uint32_t len)
{
    uint64_t newHead = r->head + len;

    /* Longer than the ring: only the tail survives. */
    if (len > r->size) {
        data += len - r->size;
        len = r->size;
    }

    uint32_t pos   = (uint32_t)((newHead - len) % r->size);
    uint32_t first = r->size - pos;
    if (first > len)
        first = len;

    memcpy(r->buf + pos, data, first);
    memcpy(r->buf, data + first, len - first);
    r->head = newHead;
}

/*
 * Copies up to outMax bytes starting at `cursor` into `out`.
 * Returns the number of bytes copied. *nextCursor is where to continue,
 * *dropped is how many bytes before `cursor` were already overwritten.
 */
static inline uint32_t rhv_ring_read(const RHVRing *r, uint64_t cursor, char *out, uint32_t outMax,
                                     uint64_t *nextCursor, uint64_t *dropped)
{
    uint64_t oldest = r->head > r->size ? r->head - r->size : 0;

    *dropped = 0;
    if (cursor < oldest) {
        *dropped = oldest - cursor;
        cursor = oldest;
    }
    if (cursor > r->head)
        cursor = r->head;

    uint64_t avail = r->head - cursor;
    uint32_t n = avail > outMax ? outMax : (uint32_t)avail;

    uint32_t pos   = (uint32_t)(cursor % r->size);
    uint32_t first = r->size - pos;
    if (first > n)
        first = n;

    memcpy(out, r->buf + pos, first);
    memcpy(out + first, r->buf, n - first);

    *nextCursor = cursor + n;
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* RYZENHV_RING_H */
