/* annexb.h -- incremental Annex-B access unit splitter.
 *
 * Deliberately free of Vita headers so it can be compiled and fuzzed on the
 * host. This is the trickiest logic in the project and the only part that
 * can be meaningfully tested without hardware, so it lives alone.
 *
 * Usage:
 *     annexb_state st;
 *     annexb_init(&st);
 *     ...
 *     int au = annexb_next_au(&st, buf, fill);
 *     if (au > 0) {
 *         decode(buf, au);
 *         memmove(buf, buf + au, fill - au);
 *         fill -= au;
 *         annexb_reset(&st);       // MUST be called after consuming
 *     }
 *
 * An access unit runs from one picture NAL (type 1 or 5) up to but not
 * including the next one. Parameter sets (7/8), AUD (9) and SEI (6) that
 * precede a picture are folded into that picture's AU, which is what the
 * decoder wants -- it needs SPS/PPS in the same submission as the IDR that
 * depends on them.
 */
#ifndef VS_ANNEXB_H
#define VS_ANNEXB_H

typedef struct {
    int scan;           /* how far we've walked into the buffer */
    int seen_picture;   /* have we passed the picture NAL of the current AU */
} annexb_state;

static void annexb_init(annexb_state *st)
{
    st->scan = 0;
    st->seen_picture = 0;
}

/* Call after consuming a returned AU and compacting the buffer.
 *
 * seen_picture goes to ZERO, not one. The retained bytes begin with the
 * start code of the next picture, so the parser must be allowed to see and
 * claim it. Setting it to 1 here makes the very next call break instantly
 * at offset 0 and return a zero-length AU forever -- the original bug this
 * file was extracted to fix. */
static void annexb_reset(annexb_state *st)
{
    st->scan = 0;
    st->seen_picture = 0;
}

static int annexb_is_picture_nal(unsigned char b)
{
    int t = b & 0x1F;
    return t == 1 || t == 5;      /* non-IDR slice, IDR slice */
}

/* Returns the byte length of a complete AU at the head of buf, or 0 if more
 * data is needed. Never returns a length of zero for a real AU. */
static int annexb_next_au(annexb_state *st, const unsigned char *buf, int len)
{
    /* Need 4 bytes to read a start code plus the NAL header byte. */
    while (st->scan + 3 < len) {
        if (buf[st->scan] == 0 && buf[st->scan + 1] == 0 &&
            buf[st->scan + 2] == 1) {

            if (annexb_is_picture_nal(buf[st->scan + 3])) {
                if (st->seen_picture) {
                    /* Boundary. Back up over a 4-byte start code prefix so
                     * the trailing zero belongs to the next AU, not this
                     * one -- otherwise every AU gains a stray byte. */
                    int end = st->scan;
                    if (end > 0 && buf[end - 1] == 0)
                        end--;
                    /* A boundary at offset 0 would mean a zero-length AU;
                     * that can only happen if reset() was misused. Guard so
                     * the caller can never be handed one. */
                    if (end <= 0) {
                        st->scan += 3;
                        continue;
                    }
                    return end;
                }
                st->seen_picture = 1;
            }
            st->scan += 3;
        } else {
            st->scan++;
        }
    }
    return 0;   /* need more bytes */
}

#endif /* VS_ANNEXB_H */
