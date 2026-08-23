/* Concurrency test: a real producer and consumer thread hammering the ring,
 * verifying every byte arrives exactly once and in order. Wrap-around is
 * where an SPSC ring goes wrong, so the buffer is deliberately tiny relative
 * to the data volume. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ring.h"

#define TOTAL (4 * 1024 * 1024)
#define RSIZE 8192

static vs_ring R;
static int fails;

static void *producer(void *arg)
{
    (void)arg;
    unsigned char chunk[1500];
    long sent = 0;
    while (sent < TOTAL) {
        int want = 1 + (rand() % (int)sizeof(chunk));
        if (sent + want > TOTAL) want = TOTAL - sent;
        for (int i = 0; i < want; i++)
            chunk[i] = (unsigned char)((sent + i) & 0xFF);   /* position seq */
        int off = 0;
        while (off < want) {
            int n = vs_ring_write(&R, chunk + off, want - off);
            if (n == 0) { struct timespec ts={0,1000}; nanosleep(&ts,0); }
            off += n;
        }
        sent += want;
    }
    vs_ring_set_eof(&R);
    return NULL;
}

int main(void)
{
    pthread_t p;
    unsigned char out[997];
    long got = 0;

    if (vs_ring_init(&R, RSIZE) < 0) return 1;
    printf("ring test: %d bytes through a %d-byte ring\n", TOTAL, RSIZE);
    pthread_create(&p, NULL, producer, NULL);

    while (got < TOTAL) {
        int n = vs_ring_read(&R, out, sizeof(out));
        if (n == 0) {
            if (vs_ring_drained(&R)) break;
            struct timespec ts={0,1000}; nanosleep(&ts,0);
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (out[i] != (unsigned char)((got + i) & 0xFF)) {
                printf("  FAIL byte %ld: got %02x want %02x\n",
                       got + i, out[i], (unsigned char)((got+i)&0xFF));
                fails++;
                if (fails > 4) return 1;
            }
        }
        got += n;
    }
    pthread_join(p, NULL);

    if (got != TOTAL) { printf("  FAIL got %ld of %d bytes\n", got, TOTAL); fails++; }
    else printf("  ok   all %ld bytes in order\n", got);

    if (vs_ring_used(&R) != 0) { printf("  FAIL ring not drained\n"); fails++; }
    else printf("  ok   ring drained\n");

    /* space/used must never exceed bounds */
    vs_ring_destroy(&R);
    vs_ring_init(&R, 64);
    unsigned char big[200]; memset(big,'z',sizeof(big));
    int w = vs_ring_write(&R, big, sizeof(big));
    printf("  %s  oversized write clamped to %d (cap %d)\n",
           w == 63 ? "ok  " : "FAIL", w, 64);
    if (w != 63) fails++;
    vs_ring_destroy(&R);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED":"PASSED", fails,
           fails==1?"":"s");
    return fails != 0;
}
