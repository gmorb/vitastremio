/* test_annexb.c -- host-side test for the access unit splitter.
 *
 * Build and run on Linux, no Vita toolchain needed:
 *     gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
 *         -o test_annexb test_annexb.c && ./test_annexb
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/annexb.h"

static int failures = 0;

static void check(const char *what, long got, long want)
{
    if (got != want) {
        printf("  FAIL %-46s got %ld want %ld\n", what, got, want);
        failures++;
    } else {
        printf("  ok   %-46s %ld\n", what, got);
    }
}

/* Drive the splitter exactly the way player.c does, recording AU sizes. */
static int run_stream(const unsigned char *stream, int total,
                      int feed_chunk, int *sizes, int max_sizes)
{
    unsigned char buf[65536];
    annexb_state st;
    int fill = 0, pos = 0, n = 0;

    annexb_init(&st);

    for (;;) {
        if (pos < total && fill < (int)sizeof(buf) - feed_chunk) {
            int take = total - pos;
            if (take > feed_chunk) take = feed_chunk;
            memcpy(buf + fill, stream + pos, take);
            fill += take;
            pos += take;
        }

        int au = annexb_next_au(&st, buf, fill);
        if (au > 0) {
            if (n < max_sizes) sizes[n] = au;
            n++;
            memmove(buf, buf + au, fill - au);
            fill -= au;
            annexb_reset(&st);
            continue;
        }
        if (pos >= total) break;   /* drained input, no more complete AUs */
    }
    return n;
}

/* Helpers to build synthetic streams. */
static int put_nal(unsigned char *p, int four_byte, unsigned char type,
                   int payload)
{
    int o = 0;
    if (four_byte) p[o++] = 0;
    p[o++] = 0; p[o++] = 0; p[o++] = 1;
    p[o++] = type;
    for (int i = 0; i < payload; i++) p[o++] = 0xAA;
    return o;
}

int main(void)
{
    unsigned char s[8192];
    int sizes[64];
    int o, n;

    printf("annexb splitter tests\n");

    /* ---- 1. The regression: three pictures must yield three AUs.
     * The original code returned a zero-length AU on the second call and
     * spun forever. If reset() is wrong this hangs or reports 1. */
    o = 0;
    o += put_nal(s + o, 1, 0x67, 12);   /* SPS */
    o += put_nal(s + o, 0, 0x68, 4);    /* PPS */
    o += put_nal(s + o, 0, 0x65, 100);  /* IDR   -> AU 1 starts */
    int au1_end = o;
    o += put_nal(s + o, 0, 0x41, 80);   /* slice -> AU 2 */
    int au2_end = o;
    o += put_nal(s + o, 0, 0x41, 60);   /* slice -> AU 3 */
    int total = o;

    n = run_stream(s, total, 4096, sizes, 64);
    check("three pictures -> AU count", n, 2);   /* last AU needs EOS to flush */
    check("AU 1 length (SPS+PPS+IDR)", sizes[0], au1_end);
    check("AU 2 length", sizes[1], au2_end - au1_end);

    /* ---- 2. No AU may ever be zero-length. */
    int zero_found = 0;
    for (int i = 0; i < n; i++) if (sizes[i] == 0) zero_found = 1;
    check("no zero-length AU emitted", zero_found, 0);

    /* ---- 3. Byte-at-a-time feeding must give identical results.
     * This is the real network case: TCP splits wherever it likes. */
    int sizes_slow[64];
    int n_slow = run_stream(s, total, 1, sizes_slow, 64);
    check("1-byte feed -> same AU count", n_slow, n);
    int same = 1;
    for (int i = 0; i < n && i < n_slow; i++)
        if (sizes[i] != sizes_slow[i]) same = 0;
    check("1-byte feed -> same AU sizes", same, 1);

    /* ---- 4. 4-byte start codes must not leak their leading zero into the
     * previous AU. Build pictures separated by 00 00 00 01. */
    o = 0;
    o += put_nal(s + o, 1, 0x65, 50);
    int b1 = o;
    o += put_nal(s + o, 1, 0x41, 50);
    int b2 = o;
    o += put_nal(s + o, 1, 0x41, 50);
    n = run_stream(s, o, 4096, sizes, 64);
    check("4-byte start codes -> AU count", n, 2);
    /* b1 is the offset where NAL 2 begins, and that first byte IS the lead
     * zero of its 4-byte start code. So AU 1 is exactly b1 bytes: the
     * splitter must back up over that zero rather than absorb it. */
    check("4-byte AU 1 excludes next lead zero", sizes[0], b1);
    check("4-byte AU 2 length", sizes[1], b2 - b1);

    /* ---- 5. Non-picture NALs alone must never emit an AU. */
    o = 0;
    o += put_nal(s + o, 1, 0x67, 20);
    o += put_nal(s + o, 0, 0x68, 8);
    o += put_nal(s + o, 0, 0x06, 30);   /* SEI */
    n = run_stream(s, o, 4096, sizes, 64);
    check("parameter sets only -> no AU", n, 0);

    /* ---- 6. Emulation-prevention bytes (00 00 03) inside a payload must
     * not be mistaken for start codes. */
    o = 0;
    o += put_nal(s + o, 1, 0x65, 0);
    s[o++] = 0; s[o++] = 0; s[o++] = 3; s[o++] = 1;   /* escaped 00 00 01 */
    s[o++] = 0xAA;
    int c1 = o;
    o += put_nal(s + o, 0, 0x41, 20);
    o += put_nal(s + o, 0, 0x41, 20);
    n = run_stream(s, o, 4096, sizes, 64);
    check("emulation prevention not split", sizes[0], c1);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
