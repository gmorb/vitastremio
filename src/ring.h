/* ring.h -- single-producer / single-consumer ring buffer.
 *
 * Why this exists: the player originally read from the socket inline with
 * decoding. The audio thread pulled exactly one 8ms grain per iteration, so
 * any network hiccup left sceAudioOutOutput with nothing to play; audio
 * stalled, the master clock stopped, and video froze with it. The video
 * thread had the same shape, and worse, it stopped reading entirely while
 * sleeping to pace a frame -- up to 42ms per frame of no socket activity.
 *
 * With a ring between them, a reader thread can keep pulling while the
 * consumer is busy, so jitter is absorbed instead of becoming a stutter.
 *
 * Lock-free and safe for exactly one reader thread and one writer thread.
 * head is written only by the producer, tail only by the consumer.
 *
 * The indices use explicit acquire/release atomics rather than volatile plus
 * a fence. Same generated code on ARM in practice, but it states the
 * ordering the algorithm actually relies on -- the consumer must not observe
 * a new head before the bytes it publishes -- instead of leaving it implied.
 *
 * Vita-header-free so it can be tested on the host.
 */
#ifndef VS_RING_H
#define VS_RING_H

#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char  *buf;
    int             size;
    int             head;      /* producer writes here */
    int             tail;      /* consumer reads here  */
    int             eof;       /* producer saw end of stream */
} vs_ring;

#define VS_LOAD_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define VS_STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static int vs_ring_init(vs_ring *r, int size)
{
    r->buf = (unsigned char *)malloc(size);
    if (!r->buf) return -1;
    r->size = size;
    r->head = r->tail = 0;
    r->eof = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

/* Reuse a buffer the caller owns, resetting the ring to empty.
 *
 * Lets the buffers be allocated once for the life of the app instead of on
 * every stream change: a megabyte-and-a-half allocation in the middle of a
 * transition is exactly the kind of work worth not doing. */
static void vs_ring_attach(vs_ring *r, unsigned char *buf, int size)
{
    r->buf  = buf;
    r->size = size;
    r->head = r->tail = 0;
    r->eof  = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void vs_ring_destroy(vs_ring *r)
{
    free(r->buf);
    r->buf = NULL;
}

/* One slot is always left empty so head==tail unambiguously means empty. */
static int vs_ring_used(const vs_ring *r)
{
    int h = VS_LOAD_ACQ(&r->head);
    int t = VS_LOAD_ACQ(&r->tail);
    return h >= t ? h - t : r->size - t + h;
}

static int vs_ring_space(const vs_ring *r)
{
    return r->size - 1 - vs_ring_used(r);
}

/* Producer side. Returns bytes actually written (may be short). */
static int vs_ring_write(vs_ring *r, const unsigned char *src, int len)
{
    int h = VS_LOAD_ACQ(&r->head);
    int space = vs_ring_space(r);
    int first;

    if (len > space) len = space;
    if (len <= 0) return 0;

    first = r->size - h;
    if (first > len) first = len;
    memcpy(r->buf + h, src, first);
    if (len > first)
        memcpy(r->buf, src + first, len - first);

    VS_STORE_REL(&r->head, (h + len) % r->size);   /* publishes the data */
    return len;
}

/* Consumer side. Returns bytes actually read (may be short). */
static int vs_ring_read(vs_ring *r, unsigned char *dst, int len)
{
    int t = VS_LOAD_ACQ(&r->tail);
    int used = vs_ring_used(r);
    int first;

    if (len > used) len = used;
    if (len <= 0) return 0;

    first = r->size - t;
    if (first > len) first = len;
    memcpy(dst, r->buf + t, first);
    if (len > first)
        memcpy(dst + first, r->buf, len - first);

    VS_STORE_REL(&r->tail, (t + len) % r->size);   /* frees the space */
    return len;
}

static void vs_ring_set_eof(vs_ring *r) { VS_STORE_REL(&r->eof, 1); }
static int  vs_ring_eof(const vs_ring *r) { return VS_LOAD_ACQ(&r->eof); }

/* True once the producer is done AND everything has been consumed. */
static int vs_ring_drained(const vs_ring *r)
{
    return vs_ring_eof(r) && vs_ring_used(r) == 0;
}

#endif /* VS_RING_H */
