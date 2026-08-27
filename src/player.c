/* player.c -- raw H.264 + raw PCM playback on PS Vita.
 *
 * Two sockets, two threads:
 *   audio thread  -- reads s16le PCM, writes to sceAudioOut. This call blocks
 *                    until the hardware wants more, so it paces itself for
 *                    free and gives us a sample-accurate clock.
 *   video thread  -- reads Annex-B, splits on start codes, feeds sceAvcdec,
 *                    then sleeps until the audio clock reaches the frame's
 *                    presentation time before publishing it.
 *
 * Audio is the master clock because PCM sample count cannot drift -- there
 * is no decoder in the path to lie to us about how much has actually played.
 *
 * The stream is forced to constant 30fps by the middleware, so frame N is
 * due at N/30 seconds. Duplicated frames from 23.976 sources cost almost
 * nothing to encode (they become skip macroblocks) and buy us the ability
 * to drop PTS parsing entirely.
 *
 * !! The sceAvcdec sequence below is the part most likely to need on-device
 * !! adjustment. Error codes are surfaced verbatim so you can look them up.
 */

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/videodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

#include "annexb.h"
#include "ring.h"
#include <psp2/display.h>
#include "log.h"
#include "http.h"
#include "player.h"

#define VID_W        960
#define VID_H        544
#define FPS          30
/* Must be >= the encoder's -refs. The middleware pins that to 3, so the
 * decoder pool stays small enough to allocate reliably from phycont. */
#define REF_FRAMES   3

#define AUD_RATE     32000
#define AUD_CH       2
/* 256 samples = 8ms per write. Smaller than the old 1024 (32ms) so the
 * master clock refreshes well inside a frame interval; the extra syscalls
 * are trivial next to decoding. */
#define AUD_GRAIN    256
#define AUD_CHUNK    (AUD_GRAIN * AUD_CH * 2)
#define AUD_GRAIN_US (AUD_GRAIN * 1000000 / AUD_RATE)

/* Fixed offset between the two streams, measured by ear on hardware.
 *
 * Audio consistently needs the video schedule to run ~215ms ahead of where
 * the sample counter says audio is. That is far more than the audio port's
 * own buffering explains, and the most likely source is the middleware: the
 * video and audio ffmpeg processes are separate invocations seeking the same
 * source independently, and -ss lands video on the nearest keyframe while
 * audio lands on the nearest audio frame. A constant content-level skew is
 * exactly what that produces.
 *
 * Folded into the clock rather than left as a trim default so that a trim of
 * 0 means "calibrated baseline" and any non-zero trim is a real per-source
 * deviation. If a particular source needs a different value, the D-pad trim
 * still adjusts on top of this.
 *
 * The +20ms: the middleware now ends its audio chain with an alimiter, whose
 * latency equals its attack exactly (20ms, confirmed by timing an impulse
 * through it). It is always in the chain -- never conditional on the boost
 * toggle -- precisely so this stays a single constant instead of shifting
 * every time the toggle is flipped. Audio arrives 20ms later, so the video
 * schedule has to run 20ms further ahead.
 *
 * !! The 235 has been reasoned from a measured filter latency, not re-tuned
 * !! by ear on hardware. If sync feels slightly off after this change, this
 * !! constant is the first thing to check. */
#define AUD_PIPELINE_OFFSET_US 235000

#define ES_BUF       (512 * 1024)         /* elementary stream scratch      */

/* Network jitter absorbers. Sized in seconds of playback rather than bytes:
 * ~2.5Mbit video and ~1Mbit audio, so 1.5MB and 256KB are roughly 4.8s and
 * 2s of cushion. Without these the consumer stalled on every hiccup. */
/* How many consecutive receive timeouts to tolerate before concluding the
 * stream is genuinely dead. Each is VS_RECV_TIMEOUT_US long, so this is
 * about a minute of patience. */
#define NET_MAX_STALLS 3

#define RING_V       (1536 * 1024)
#define RING_A       (256 * 1024)

/* How much to bank before starting. Audio is the master clock, so its
 * cushion is what actually prevents the clock from stalling. */
#define PREBUF_V     (384 * 1024)
#define PREBUF_A     (64 * 1024)
#define PREBUF_MAX_WAIT_US (12 * 1000000)

#define NUM_FRAMES 4

/* Most the schedule watchdog will shift the anchor in one go, in refreshes.
 * Half a second is more than any real scheduling error and far less than a
 * supply deficit, which is the distinction that matters: the first is worth
 * correcting, the second cannot be corrected here at all. */
#define WATCHDOG_MAX_LEAD 30

/* How late a decoded frame must be, against the audio clock, before the
 * backstop presents it without consulting the vsync schedule. Comfortably
 * beyond any legitimate scheduling jitter -- a frame is normally shown
 * within one refresh of its time -- and well inside what a viewer would
 * call out of sync. */
#define BACKSTOP_LATE_US 250000

/* state: 0 free (decoder may claim), 1 ready (main may show/release) */
typedef struct {
    vita2d_texture *tex;
    long            pts_us;
    long            index;     /* frame number, drives the vsync schedule */
    int             state;
} frame_slot;

#define SLOT_FREE  0
#define SLOT_READY 1

typedef struct {
    /* --- shared state, guarded by lock --- */
    SceUID          lock;
    volatile int    running;
    volatile int    quit_requested;
    volatile long   samples_played;       /* audio master clock             */
    SceUInt64          clock_epoch;       /* timer origin for this playback  */
    volatile long      clock_offset_us;   /* sample-counter minus timer       */
    volatile int       clock_locked;
    int                clock_samples;

    /* Pause bookkeeping. Both clocks must freeze together: the audio clock
     * runs off the free-running system timer, and the video schedule runs
     * off the vblank counter. Neither stops on its own just because we stop
     * consuming, so paused time is subtracted from one and added to the
     * other's anchor. */
    volatile int       paused;
    SceUInt64          pause_started;
    volatile long      pause_accum_us;

    /* Set while the audio ring is dry and the clock is held still. Read by
     * the UI to draw a buffering indicator; see stall_begin/stall_end. */
    volatile int       buffering;
    SceUInt64          stall_started;
    volatile long   frames_dropped;
    volatile int    fps_milli;            /* source rate x1000, from header  */
    volatile long   duration_s;           /* whole stream, from header, 0=?  */

    /* --- connection --- */
    const char     *ip;
    int             port;
    /* Must hold "/v?s=" + a key up to 2048 bytes + "&t=NNNN". */
    char            vpath[2176];
    char            apath[2176];

    /* --- video --- */
    SceUID          frame_memblk;
    int             vdec_lib_init;   /* videodec library is initialised */
    int             vdec_created;    /* a decoder instance exists */
    SceAvcdecCtrl   ctrl;
    /* Frame queue. The decoder fills slots and never sleeps to pace; the
     * main thread picks which slot to show once per vblank. Deciding on the
     * vsync boundary is what makes the cadence regular -- flipping whenever
     * the audio clock happened to reach a PTS landed at random phase against
     * the 60Hz grid and produced irregular judder. */
    frame_slot      frames[NUM_FRAMES];
    volatile int    cur_slot;        /* what main is displaying, -1 = none */
    int             pending_free;    /* released one switch late, see switch_to */
    volatile long   presented;
    volatile long   repeats;

    /* Vsync-locked schedule. Presentation is driven by counting refreshes,
     * not by comparing against the audio clock: that clock jitters by up to
     * one audio grain (8ms), which is enough to flip a 2-refresh frame into
     * a 3-refresh frame and back. The average stayed correct while the
     * cadence was irregular -- which is what judder actually is. */
    long            vsync_n;
    long            anchor;      /* refresh at which frame 0 would show */

    /* Measured refresh rate x1000. The schedule originally assumed exactly
     * 60.000 Hz, but the panel runs at 59.94 like most NTSC-lineage
     * displays. That 0.1% error made video lose 1ms per second against
     * audio, so the offset drifted steadily negative and had to be yanked
     * back by a correction every ~20s -- visible as wandering lip sync and a
     * slowly climbing irregular count.
     *
     * Measured rather than hardcoded: it costs nothing and covers whatever
     * the hardware actually does.
     *
     * Worth noting why this matters so much here: 23.976 = 24000/1001 and
     * 59.94 = 60000/1001, so their ratio is exactly 2.5. With the true rate
     * the cadence is a perfect 2,3,2,3 that never needs correcting at all. */
    long            hz_micro;   /* rate x1e6; milli-Hz truncated small steps */
    int             vcount_base;
    long            rate_n0;
    long            rate_av0;
    long            rate_window;
    long            shown_index; /* frame number currently on screen */
    int             hold;        /* refreshes the current frame has held */
    long            hold_hist[6];/* how often a frame held 1..5+ refreshes */
    long            resyncs;
    volatile long   av_offset_us;/* smoothed; + = video ahead of audio */
    volatile long   av_trim_us;  /* user target offset, tuned by ear */
    int             av_valid;
    long            last_correct_vsync;
    long            last_present_vsync;
    long            last_watchdog_vsync;
    long            last_snap_vsync;

    /* Vblank counter health.
     *
     * counter_cold means the counter is not currently moving and pacing is
     * coming from the system clock; it clears by itself the moment the
     * counter advances again. Per-session state in P rather than statics --
     * statics survived across streams and carried one session's verdict
     * into the next. */
    int             vcount_last;      /* last raw reading, for the delta */
    SceUInt64       vcount_last_us;   /* when it last actually moved */
    SceUInt64       tick_last_us;     /* timer accumulator, keeps fractions */
    int             counter_cold;
    int             backstop_fired;   /* log once per session, not per frame */

    SceUID          vthread, athread;
    SceUID          vnet, anet;

    /* Reader sockets, published so vs_play_stop can close them.
     *
     * Setting quit_requested is not enough: a reader sits inside sceNetRecv
     * with a 20-second timeout and does not look at the flag until that
     * returns. Joining the threads therefore blocked the MAIN thread for up
     * to 20s, with no vita2d_swap_buffers in that window -- a starved
     * display queue, which is what a GPU watchdog exists to catch.
     *
     * Closing the socket makes the blocked recv return at once. Exchanged
     * atomically so the reader and the stopper cannot both close it. */
    volatile int    net_fd[2];      /* 0 = video, 1 = audio */

    vs_ring         ring_v, ring_a;
    volatile int    prebuffered;
    volatile long   underruns;
} vs_player;

static vs_player P;

/* User trim on top of the built-in pipeline offset, which is now folded into
 * the clock itself (see AUD_PIPELINE_OFFSET_US). Default 0, so the overlay
 * reads trim +0ms when everything is at its calibrated baseline and any
 * value here represents a real deviation from it.
 *
 * Kept outside P so it survives the memset in vs_play_start; retuning after
 * every seek would defeat the point. */
#define AV_TRIM_DEFAULT_US 0

static long g_saved_trim_us = AV_TRIM_DEFAULT_US;

/* Chosen audio stream, -1 for the file's default. Outside P for the same
 * reason as the trim: vs_play_start memsets P, and switching tracks works by
 * restarting playback, so it has to survive that. */
static int g_audio_track = -1;
static int g_boost       = 0;
static char g_content_id[80];

/* ------------------------------------------------------- texture graveyard
 *
 * Frame textures are not freed the instant playback stops. vita2d has no way
 * to tell us the GPU is finished with a specific texture, and
 * vita2d_wait_rendering_done only covers the scene currently being built --
 * a buffer queued for display can still reference a texture for another
 * frame or two. Freeing under the GPU is an unrecoverable fault rather than
 * a crash we can catch.
 *
 * Holding them for a few frames costs 8MB briefly and removes the race
 * entirely. */
/* ---------------------------------------------------- persistent buffers
 *
 * The decode target is always 960x544, and the rings are always the same
 * size, so none of this has any reason to be reallocated when the stream
 * changes. Allocating once removes the whole burst -- 8MB of GPU textures
 * mapped into the GPU MMU, plus 2.25MB of heap -- from the moment a source
 * is selected, which is precisely when the CPU was spiking and the GPU was
 * faulting.
 *
 * It also removes, by construction, every free-under-the-GPU hazard that
 * the deferred-release graveyard existed to paper over.
 */
static vita2d_texture *g_pool_tex[NUM_FRAMES];
static unsigned char  *g_pool_ring_v;
static unsigned char  *g_pool_ring_a;
static unsigned char  *g_pool_es;

static int pool_alloc(void)
{
    if (!g_pool_ring_v) g_pool_ring_v = (unsigned char *)malloc(RING_V);
    if (!g_pool_ring_a) g_pool_ring_a = (unsigned char *)malloc(RING_A);
    if (!g_pool_es)     g_pool_es     = (unsigned char *)malloc(ES_BUF);
    if (!g_pool_ring_v || !g_pool_ring_a || !g_pool_es) {
        vs_log("pool: heap allocation failed");
        return -1;
    }

    for (int i = 0; i < NUM_FRAMES; i++) {
        if (g_pool_tex[i]) continue;
        g_pool_tex[i] = vita2d_create_empty_texture_format(
                            VID_W, VID_H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        if (!g_pool_tex[i]) {
            vs_log("pool: frame texture %d of %d failed", i + 1, NUM_FRAMES);
            return -1;
        }
    }
    return 0;
}

/* Blank every frame texture.
 *
 * vita2d_create_empty_texture_format does not clear what it hands back, and
 * the pool is allocated once and reused for every stream after it -- so a
 * slot still holds whatever the LAST film left there. Any moment a slot is
 * on screen before its first real decode lands shows that: a flash of the
 * previous stream, or of uninitialised memory on the very first play, which
 * is the momentary garbage seen when starting a stream.
 *
 * Wait for the GPU first. These are live textures; memsetting one the GPU is
 * still scanning out is how you fault it, and a GXM fault here is a hard
 * hang rather than a crash back to the LiveArea. */
static void pool_blank(void)
{
    vita2d_wait_rendering_done();
    for (int i = 0; i < NUM_FRAMES; i++) {
        void *p;
        if (!g_pool_tex[i]) continue;
        p = vita2d_texture_get_datap(g_pool_tex[i]);
        if (p) memset(p, 0, (size_t)VID_W * VID_H * 4);
    }
}



/* Retained as a no-op: the persistent pool means there is nothing to
 * collect, but the main loop calls this every frame and keeping the symbol
 * avoids churn if buffers ever become dynamic again. */
void vs_play_gc(void) { }

void vs_play_set_audio_track(int idx) { g_audio_track = idx; }
void vs_play_set_boost(int on)        { g_boost = on ? 1 : 0; }
int  vs_play_buffering(void)
{ return __atomic_load_n(&P.buffering, __ATOMIC_ACQUIRE); }
int  vs_play_boost(void)              { return g_boost; }

void vs_play_set_content_id(const char *id)
{
    snprintf(g_content_id, sizeof(g_content_id), "%s", id ? id : "");
}
int  vs_play_audio_track(void)        { return g_audio_track; }

/* ------------------------------------------------------------------ clock */

/* The raw sample counter only advances once per audio grain -- every 32ms
 * at 1024 samples. Video frames are 33-42ms apart, so a frame waiting on the
 * bare counter always lands on the next 32ms boundary, quantising playback
 * into an uneven stutter that looks exactly like dropped frames.
 *
 * Interpolating with the system microsecond timer between grain writes gives
 * a continuous clock while keeping the sample count as the drift-free
 * reference. The elapsed term is clamped to one grain so a stalled audio
 * thread can't run the clock away and make video sprint. */
/* Audio clock, phase-locked to the system timer.
 *
 * Reading position straight off samples_played means reading a value that
 * only moves once per 8ms grain, then interpolating with a clamp. That
 * carries several ms of quantisation noise into every sync measurement --
 * which is most of what makes the reported offset jump around.
 *
 * Audio hardware consumes samples at a fixed rate, so the difference between
 * the sample counter and the system timer is very nearly constant. Tracking
 * that offset with a long average and reading the clock off the free-running
 * system timer gives a smooth, continuous clock: the grain quantisation
 * averages out of the offset instead of appearing in every reading.
 *
 * The offset still follows genuine changes (an underrun, a seek), just
 * slowly enough that quantisation noise does not survive. */
/* Elapsed playing time since the clock epoch, excluding paused spans.
 *
 * Both clocks have to stop together on pause. This one runs off the
 * free-running system timer, so paused wall time is subtracted here; the
 * video schedule runs off the vblank counter and is held by advancing its
 * anchor instead. */
static long clock_now_rel(void)
{
    SceUInt64 now = sceKernelGetProcessTimeWide();
    long      rel;

    if (P.clock_epoch == 0) return 0;
    rel = (long)(now - P.clock_epoch) - P.pause_accum_us;

    if (P.paused && P.pause_started)
        rel -= (long)(now - P.pause_started);
    return rel;
}

static long audio_clock_us(void)
{
    /* samples_played counts samples HANDED TO the audio port, but the
     * hardware is still working through the previously queued grain when
     * sceAudioOutOutput returns. Reporting the queued position would put the
     * clock ahead of what is audible, and video scheduled against it would
     * lead the sound -- heard as audio lagging. Back off one grain so the
     * clock tracks what the listener is actually hearing. */
    long now_rel;

    if (P.clock_epoch == 0) return 0;
    now_rel = clock_now_rel();

    if (!P.clock_locked) {
        /* Before the filter has settled, fall back to the raw counter so
         * startup alignment still has something to work with. */
        return (long)((double)P.samples_played * 1000000.0 / AUD_RATE)
               - AUD_GRAIN_US + AUD_PIPELINE_OFFSET_US;
    }
    return now_rel + P.clock_offset_us + AUD_PIPELINE_OFFSET_US;
}

/* Freeze and unfreeze the master clock around an audio underrun.
 *
 * Once the phase-locked loop settles, audio_clock_us() returns a value
 * derived from the SYSTEM TIMER, not from samples_played. That is what makes
 * it smooth, but it also means the clock keeps advancing when audio stops:
 * the ring runs dry, the audio thread blocks, and the video schedule -- which
 * paces against that clock -- carries on regardless. Picture ran ahead of
 * sound for the length of the stall, and then the /128 filter took several
 * seconds to absorb the error afterwards.
 *
 * Holding the clock still for the duration is the fix, and the machinery
 * already exists: pause_accum_us is exactly "time that must not count".
 * Video then waits with the audio instead of running away from it, and
 * because nothing ever went out of alignment there is nothing to resync.
 *
 * Deliberately NOT reusing P.paused: pause is a user action with its own
 * semantics elsewhere (the audio thread stops consuming when it is set,
 * which here would be a deadlock). This only borrows its accumulator. */
/* Defined with the rate estimator further down; needed here because a
 * buffering stall invalidates any window open across it. */
static void rate_reset(void);

static void stall_begin(void)
{
    if (P.buffering) return;
    P.stall_started = sceKernelGetProcessTimeWide();
    __atomic_store_n(&P.buffering, 1, __ATOMIC_RELEASE);
}

static void stall_end(void)
{
    if (!P.buffering) return;
    P.pause_accum_us += (long)(sceKernelGetProcessTimeWide()
                               - P.stall_started);
    P.stall_started = 0;
    __atomic_store_n(&P.buffering, 0, __ATOMIC_RELEASE);

    /* The clock was held still across the stall, so any rate window open
     * at the time spans a gap where measured time and real time diverged.
     * Reading a slope from that produced corrections of entirely the wrong
     * size -- the estimator concluded the clocks ran at different speeds
     * and skewed hz_micro, after which the offset grew rather than closed.
     * Discard the window; the estimate itself is still good. */
    rate_reset();
}

/* Called from the audio thread each time a grain is handed to the hardware. */
static void audio_clock_update(void)
{
    SceUInt64 now = sceKernelGetProcessTimeWide();
    long      now_rel, from_samples, inst;

    if (P.clock_epoch == 0) {
        P.clock_epoch = now;
        return;
    }
    now_rel      = clock_now_rel();
    from_samples = (long)((double)P.samples_played * 1000000.0 / AUD_RATE)
                   - AUD_GRAIN_US;
    inst         = from_samples - now_rel;

    if (!P.clock_locked) {
        P.clock_offset_us = inst;
        /* Half a second of grains before trusting the filter. */
        if (++P.clock_samples > 64) P.clock_locked = 1;
    } else {
        /* Long average: 1/128 per grain is ~1s of smoothing, which buries
         * the 8ms quantisation while still tracking real drift. */
        P.clock_offset_us += (inst - P.clock_offset_us) / 128;
    }
}

/* --------------------------------------------------------- network readers
 *
 * These exist so the socket is drained continuously regardless of what the
 * consumer is doing. The video thread in particular sleeps up to 42ms per
 * frame to pace playback; previously nothing was reading during that window
 * and the stream fell behind on any jitter.
 */
static void net_close(int which)
{
    int fd = __atomic_exchange_n(&P.net_fd[which], -1, __ATOMIC_ACQ_REL);
    if (fd >= 0) sceNetSocketClose(fd);
}

static int net_reader(vs_ring *ring, const char *path)
{
    vs_conn c;
    unsigned char buf[16384];
    int n, off, w, stalls = 0;
    int which = (ring == &P.ring_v) ? 0 : 1;

    if (vs_get(&c, P.ip, P.port, path) < 0) {
        vs_log("net reader: connect failed for %s", path);
        vs_ring_set_eof(ring);
        return -1;
    }
    /* Publish the socket so a stop can interrupt a blocked recv. */
    __atomic_store_n(&P.net_fd[which], c.fd, __ATOMIC_RELEASE);

    /* Hand the stream headers back to the video thread. */
    if (ring == &P.ring_v) {
        if (c.fps_milli >= 1000) P.fps_milli  = c.fps_milli;
        if (c.duration_s  >  0)  P.duration_s = c.duration_s;
    }

    while (P.running && !P.quit_requested) {
        n = vs_read(&c, buf, sizeof(buf));

        if (n == 0) break;              /* clean end of stream */

        if (n < 0) {
            /* Timeout or transient error. Remote sources stall for tens of
             * seconds under load and then resume, so retry rather than
             * declaring the stream finished -- treating this as EOF is what
             * made playback freeze until a seek restarted the transcode. */
            if (++stalls > NET_MAX_STALLS) {
                vs_log("%s: giving up after %d stalls (%ds)",
                       (ring == &P.ring_v) ? "video" : "audio",
                       stalls, stalls * (VS_RECV_TIMEOUT_US / 1000000));
                break;
            }
            vs_log("%s: stalled, retrying (%d/%d)",
                   (ring == &P.ring_v) ? "video" : "audio",
                   stalls, NET_MAX_STALLS);
            continue;
        }
        stalls = 0;                     /* progress resets the counter */

        off = 0;
        while (off < n && P.running && !P.quit_requested) {
            w = vs_ring_write(ring, buf + off, n - off);
            off += w;
            if (w == 0) sceKernelDelayThread(2000);   /* ring full: consumer
                                                       * is behind, back off */
        }
    }
    net_close(which);       /* whichever of us gets there first */
    c.fd = -1;              /* vs_close must not close it again */
    vs_close(&c);
    vs_ring_set_eof(ring);
    return 0;
}

static int vnet_thread(SceSize a, void *p)
{ (void)a; (void)p; return net_reader(&P.ring_v, P.vpath); }

static int anet_thread(SceSize a, void *p)
{ (void)a; (void)p; return net_reader(&P.ring_a, P.apath); }

/* Block until both rings have banked enough, or we give up. */
static void wait_for_prebuffer(void)
{
    SceUInt64 start = sceKernelGetProcessTimeWide();

    while (P.running && !P.quit_requested) {
        int v = vs_ring_used(&P.ring_v);
        int a = vs_ring_used(&P.ring_a);

        if (v >= PREBUF_V && a >= PREBUF_A) break;
        /* A short stream can legitimately end before filling the buffer. */
        if (vs_ring_eof(&P.ring_v) || vs_ring_eof(&P.ring_a)) break;
        if (sceKernelGetProcessTimeWide() - start > PREBUF_MAX_WAIT_US) {
            vs_log("prebuffer timed out at v=%d a=%d; starting anyway", v, a);
            break;
        }
        sceKernelDelayThread(20000);
    }
    vs_log("prebuffered v=%d a=%d bytes",
           vs_ring_used(&P.ring_v), vs_ring_used(&P.ring_a));
    P.prebuffered = 1;
}

/* ------------------------------------------------------------ audio thread */

static int audio_thread(SceSize args, void *argp)
{
    int   port, have;
    char *buf = malloc(AUD_CHUNK);

    (void)args; (void)argp;
    if (!buf) return 0;

    wait_for_prebuffer();

    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUD_GRAIN,
                               AUD_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { vs_log("audio port failed 0x%08X", port);
                    free(buf); P.running = 0; return 0; }

    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH |
                               SCE_AUDIO_VOLUME_FLAG_R_CH,
                         (int[]){ SCE_AUDIO_VOLUME_0DB,
                                  SCE_AUDIO_VOLUME_0DB });

    while (P.running && !P.quit_requested) {
        if (P.paused) {
            /* Stop consuming. The rings back up, the reader threads block on
             * a full ring, and the middleware's ffmpeg blocks on the socket
             * -- so a pause costs nothing upstream either. */
            sceKernelDelayThread(10000);
            continue;
        }

        /* Assemble exactly one grain; sceAudioOutOutput demands a full one. */
        have = 0;
        while (have < AUD_CHUNK) {
            int n = vs_ring_read(&P.ring_a,
                                 (unsigned char *)buf + have, AUD_CHUNK - have);
            if (n > 0) { have += n; continue; }

            if (vs_ring_drained(&P.ring_a)) goto done;
            /* Ring momentarily empty. Waiting here is correct -- emitting
             * silence would advance the master clock past audio that is
             * merely late, desyncing video permanently.
             *
             * Waiting is not sufficient on its own, though: the clock runs
             * off the system timer once locked, so it advanced through the
             * wait even with audio stopped. Freeze it for the duration. */
            if (!P.buffering) P.underruns++;
            stall_begin();
            sceKernelDelayThread(2000);
            if (!P.running || P.quit_requested) goto done;
        }

        /* Hysteresis. Resuming the instant one grain arrives would stall
         * again on the next, so wait for a real cushion -- a quarter of the
         * startup prebuffer -- before letting the clock run. Without this a
         * marginal connection oscillates in and out of buffering rather
         * than pausing once and recovering.
         *
         * Waits in place rather than restarting the outer loop: buf already
         * holds a complete grain read out of the ring, and going back to the
         * top would zero `have` and read another, silently dropping this
         * one -- an audible gap every time buffering ended. */
        if (P.buffering) {
            while (P.running && !P.quit_requested
                   && vs_ring_used(&P.ring_a) < PREBUF_A / 4
                   && !vs_ring_eof(&P.ring_a))
                sceKernelDelayThread(2000);
            stall_end();
        }

        if (sceAudioOutOutput(port, buf) < 0) break;
        P.samples_played += AUD_GRAIN;
        audio_clock_update();
    }

done:
    sceAudioOutReleasePort(port);
    free(buf);
    P.running = 0;
    return 0;
}

/* ------------------------------------------------------------ video decode */

/* Undo exactly what decoder_open managed to do, in reverse order.
 *
 * Every failure path used to return without unwinding, so a partly-opened
 * decoder left the videodec library initialised and a physically contiguous
 * block allocated. Both are scarce: after a couple of stream switches the
 * phycont pool is gone, and re-initialising an already-initialised library
 * is exactly the kind of thing that takes the GPU down with it. */
static void decoder_close(void)
{
    if (P.vdec_created) {
        sceAvcdecDeleteDecoder(&P.ctrl);
        P.vdec_created = 0;
    }
    if (P.vdec_lib_init) {
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        P.vdec_lib_init = 0;
    }
    if (P.frame_memblk >= 0) {
        sceKernelFreeMemBlock(P.frame_memblk);
        P.frame_memblk = -1;          /* never free the same block twice */
    }
    memset(&P.ctrl, 0, sizeof(P.ctrl));
}

static int decoder_open(void)
{
    SceVideodecQueryInitInfoHwAvcdec init;
    SceAvcdecQueryDecoderInfo  q;
    SceAvcdecDecoderInfo       info;
    void *base = NULL;
    int   ret;

    P.frame_memblk  = -1;
    P.vdec_lib_init = 0;
    P.vdec_created  = 0;

    memset(&init, 0, sizeof(init));
    init.size            = sizeof(init);
    init.horizontal      = VID_W;
    init.vertical        = VID_H;
    init.numOfRefFrames  = REF_FRAMES;
    init.numOfStreams    = 1;

    ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC,
                                 (const SceVideodecQueryInitInfoHwAvcdec *)&init);
    if (ret < 0) { vs_log("initLibrary failed 0x%08X", ret); return -1; }
    P.vdec_lib_init = 1;

    memset(&q, 0, sizeof(q));
    q.horizontal     = VID_W;
    q.vertical       = VID_H;
    q.numOfRefFrames = REF_FRAMES;

    ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &info);
    if (ret < 0) {
        vs_log("queryMemSize failed 0x%08X", ret);
        decoder_close();
        return -1;
    }

    /* The decoder frame buffer must be physically contiguous. */
    {
        SceUInt32 sz = (info.frameMemSize + 0xFFFFF) & ~0xFFFFF;
        P.frame_memblk = sceKernelAllocMemBlock(
            "avcdec_frame",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
        if (P.frame_memblk < 0) {
            vs_log("allocMemBlock failed 0x%08X (phycont exhausted?)",
                   P.frame_memblk);
            P.frame_memblk = -1;
            decoder_close();
            return -1;
        }

        /* Checked, because the consequence of not checking is severe: base
         * stays uninitialised and gets handed to the hardware decoder as the
         * buffer to write frames into. That is a DMA write to a garbage
         * physical address, which takes the whole system down rather than
         * failing cleanly. */
        ret = sceKernelGetMemBlockBase(P.frame_memblk, &base);
        if (ret < 0 || !base) {
            vs_log("getMemBlockBase failed 0x%08X", ret);
            decoder_close();
            return -1;
        }

        memset(&P.ctrl, 0, sizeof(P.ctrl));
        P.ctrl.frameBuf.pBuf = base;
        P.ctrl.frameBuf.size = sz;
    }

    ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &P.ctrl, &q);
    if (ret < 0) {
        vs_log("createDecoder failed 0x%08X", ret);
        decoder_close();
        return -1;
    }
    P.vdec_created = 1;
    return 0;
}

/* Feed one access unit; on success copy the picture into the back texture. */
static int decode_au(unsigned char *au, int len, long pts_us, int slot)
{
    SceAvcdecAu           in;
    SceAvcdecArrayPicture arr;
    SceAvcdecPicture      pic;
    SceAvcdecPicture     *plist[1] = { &pic };
    int ret;

    memset(&in,  0, sizeof(in));
    memset(&pic, 0, sizeof(pic));
    memset(&arr, 0, sizeof(arr));

    in.pts.lower  = (SceUInt32)pts_us;
    in.dts.lower  = (SceUInt32)pts_us;
    in.es.pBuf    = au;
    in.es.size    = len;

    pic.size                 = sizeof(pic);
    pic.frame.pixelType      = 0;          /* 0 = RGBA8888 on this decoder */
    pic.frame.framePitch     = VID_W;
    pic.frame.frameWidth     = VID_W;
    pic.frame.frameHeight    = VID_H;
    /* A NULL destination here would be a hardware write to address zero.
     * Cannot happen with the current slot logic, but the cost of being wrong
     * is a system fault rather than a dropped frame. */
    if (!P.frames[slot].tex) return -1;
    pic.frame.pPicture[0]    = vita2d_texture_get_datap(P.frames[slot].tex);

    arr.numOfElm  = 1;
    arr.pPicture  = plist;

    ret = sceAvcdecDecode(&P.ctrl, &in, &arr);
    if (ret < 0) return ret;
    return arr.numOfOutput > 0 ? 1 : 0;
}

/* ------------------------------------------------------------ video thread */

#define MAX_CONSEC_ERRORS 30      /* ~1s of solid failure before giving up */
/* Late-frame dropping now lives in the presenter, which is the only place
 * that knows what is already on screen. */

/* Lip-sync tolerance.
 *
 * A correction moves the schedule by one refresh, S = 16.7ms, and it lands
 * on the OPPOSITE side of zero. So the condition for not re-triggering
 * immediately is |T - S| <= T, i.e. T >= S/2 = 8.4ms -- not T >= S as an
 * earlier version of this comment claimed. 10ms leaves a little margin above
 * that floor and bounds the offset to about +/-9ms. Sitting this close to
 * the 8.34ms limit is only safe because the phase-locked clock removed the
 * noise that would otherwise trip it spuriously.
 *
 * Below ~8.4ms it cannot settle: every correction overshoots the threshold
 * on the far side and triggers another. Going tighter would need sub-refresh
 * adjustment, which means resampling audio -- not worth it, since 10ms is
 * already far under the ~45ms where sound trailing picture is detectable. */
#define AV_CORRECT_US 9000

/* Above this the error is taken out in one correction rather than nudged.
 *
 * 150ms is well past anything ordinary drift produces between corrections,
 * and well past the ~45ms where sound trailing picture becomes noticeable --
 * so by the time this triggers the viewer can already see the problem, and a
 * single jump is the least bad way out of it. */
#define AV_SNAP_US 150000

/* Refreshes a snap is given to play out before another may fire.
 *
 * A snap moves the schedule and lets the refresh count catch up, which takes
 * as long as the error was large -- three seconds of error needs three
 * seconds to absorb. Measuring again before then sees the same error still
 * present and fires again, and again, each one re-presenting a frame. The
 * result is roughly 1fps video while the sound plays normally. Five seconds
 * covers any snap the AV_SNAP_US threshold can produce in practice. */
#define AV_SNAP_COOLDOWN 300

/* Corrections are decided on a smoothed offset, not a raw sample.
 *
 * The audio clock jitters by up to one grain, so individual readings scatter
 * by tens of milliseconds. Acting on raw samples fires spurious corrections
 * -- each costing one irregular hold -- while the real offset sits still.
 * With the phase-locked audio clock the input is already quiet, so what this
 * mainly has to smooth is the +/-8.3ms presentation sawtooth -- the
 * unavoidable consequence of frames landing on refresh boundaries when their
 * ideal times fall between them.
 *
 * 32 measured better than 8, 16 or 24 across a grid: shorter averages let
 * the sawtooth through and widened the settled range from 19ms to 30ms. */
#define AV_EMA_DIV 32

/* The estimation window doubles after each adjustment, from 8s up to 60s.
 *
 * A short first window catches the initial error quickly; longer windows
 * then refine the estimate, because slope accuracy scales with baseline
 * length. Starting long instead (30s+) leaves the rate uncorrected while it
 * drifts, and a fixed short window never gets accurate -- measured over ten
 * minutes, fixed-8s settles 800ppm off with 64 corrections, while doubling
 * to 60s settles within 20ppm with 8. */
#define RATE_WINDOW_START   480      /* ~8s  */
#define RATE_WINDOW_MAX     3600     /* ~60s */
#define RATE_MAX_STEP_PPM   6000     /* clamp one adjustment to 0.6% */


static int video_thread(SceSize args, void *argp)
{
    unsigned char *es = g_pool_es;
    annexb_state st;
    int  fill = 0, n;
    long long frame_index = 0;
    int  consec_errors = 0;
    int  input_done = 0;

    (void)args; (void)argp;

    if (!es) return 0;
    if (decoder_open() < 0) {
        decoder_close();        /* idempotent; unwinds any partial state */
        P.running = 0;
        return 0;
    }

    /* The network thread has already opened the socket and captured the
     * framerate header by the time prebuffering completes. */
    while (!P.prebuffered && P.running && !P.quit_requested)
        sceKernelDelayThread(5000);

    vs_log("video stream at %d.%03d fps",
           P.fps_milli / 1000, P.fps_milli % 1000);

    annexb_init(&st);

    while (P.running && !P.quit_requested) {
        int au;

        /* Top up from the ring, never the socket -- the reader thread owns
         * that and keeps draining while we sleep to pace a frame. */
        if (!input_done) {
            if (fill < ES_BUF - 65536) {
                n = vs_ring_read(&P.ring_v, es + fill, 65536);
                if (n > 0)
                    fill += n;
                else if (vs_ring_drained(&P.ring_v))
                    input_done = 1;
            } else if (vs_ring_drained(&P.ring_v)) {
                /* MUST be checked outside the fill gate.
                 *
                 * When the gate was the only place input_done was set, a
                 * full buffer meant end-of-stream was never noticed: the
                 * loop below could not take its break, so it slept 2ms and
                 * spun until the app was killed. */
                input_done = 1;
            }
        }

        au = annexb_next_au(&st, es, fill);
        if (au <= 0) {
            if (input_done) break;      /* stream ended mid-AU */

            /* DEADLOCK GUARD.
             *
             * annexb_next_au only returns an AU once it has found the NEXT
             * picture NAL, so it always needs bytes beyond the current
             * frame. The top-up above stops at ES_BUF - 65536. Together
             * those mean a buffer that fills without yielding an AU can
             * never be resolved: the bytes that would complete it are
             * exactly the ones no longer being read.
             *
             * The result was a permanent freeze -- video stopped, audio
             * carried on from its own ring, and pause did nothing because
             * the thread was stuck on data rather than on scheduling. Only
             * restarting the stream cleared it.
             *
             * Recovery: drop everything up to the next start code and
             * resync. That sacrifices one access unit, which costs a
             * visible glitch, against a freeze that costs the session. */
            if (fill >= ES_BUF - 65536) {
                int cut = -1;
                for (int i = 4; i + 3 < fill; i++) {
                    if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1) {
                        cut = i;
                        break;
                    }
                }
                if (cut > 0) {
                    vs_log("ES buffer full with no complete AU (%d bytes); "
                           "resyncing at +%d", fill, cut);
                    memmove(es, es + cut, fill - cut);
                    fill -= cut;
                } else {
                    /* No start code anywhere in 448KB. The stream is not
                     * H.264 as we understand it; keeping the tail is
                     * pointless and keeping the head guarantees a repeat. */
                    vs_log("ES buffer full with no start code (%d bytes); "
                           "discarding", fill);
                    fill = 0;
                }
                annexb_reset(&st);
                continue;
            }

            /* MUST sleep here.
             *
             * This used to sleep only when fill was under 8192, so once the
             * buffer held a partial AU larger than that and the ring ran
             * dry, the loop spun with no delay at all. That burns a core at
             * the same priority as the reader thread that would refill the
             * ring, so the starvation sustains itself: video stops, audio
             * keeps playing from its own ring, and only restarting the
             * stream breaks the cycle. */
            sceKernelDelayThread(2000);
            continue;
        }

        {
            /* Exact rational timing from the source rate the middleware
             * reported. Rounding 23.976 to 24 would drift ~3.6s per hour. */
            long pts_us = (long)(frame_index * 1000000000LL / P.fps_milli);
            int  slot = -1, r;

            /* Claim a FREE slot, waiting if the queue is full.
             *
             * This wait is the ONLY thing throttling the decoder, and it is
             * load-bearing. Assigning slots round-robin as frame_index %
             * NUM_FRAMES never blocks, so the decoder consumes the stream as
             * fast as the network delivers it -- playback runs at download
             * speed rather than real time -- while also overwriting frames
             * the presenter is still displaying and rewriting pts_us
             * underneath it.
             *
             * Blocking here is correct: a full queue means the presenter has
             * not consumed yet, i.e. we are comfortably ahead of real time
             * and should stop. */
            {
                int waits = 0;
                while (P.running && !P.quit_requested) {
                    for (int i = 0; i < NUM_FRAMES; i++) {
                        if (__atomic_load_n(&P.frames[i].state,
                                            __ATOMIC_ACQUIRE) == SLOT_FREE) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot >= 0) break;

                    /* Normal when running ahead. Pathological if it lasts,
                     * so leave a trail rather than silently hanging. */
                    if (++waits == 1000)          /* ~2s */
                        vs_log("decoder waiting on a free slot: cur=%d "
                               "pending=%d shown=%ld frame=%lld",
                               P.cur_slot, P.pending_free, P.shown_index,
                               frame_index);
                    sceKernelDelayThread(2000);
                }
            }
            if (slot < 0) break;                  /* stopping */

            r = decode_au(es, au, pts_us, slot);

            if (r < 0) {
                /* A decoder that rejects one AU often recovers at the next
                 * keyframe, so skip rather than abort. But a decoder that
                 * rejects everything will otherwise spin silently forever. */
                if (++consec_errors == 1 || consec_errors % 10 == 0)
                    vs_log("decode 0x%08X frame %lld (%d consecutive)",
                           r, frame_index, consec_errors);
                if (consec_errors >= MAX_CONSEC_ERRORS) {
                    vs_log("giving up after %d consecutive decode errors", consec_errors);
                    break;
                }
            } else {
                consec_errors = 0;
            }

            if (r == 1) {
                /* Publish. The presenter decides when (and whether) to show
                 * it -- dropping late frames is its job now, since only it
                 * knows what is already on screen. */
                P.frames[slot].pts_us = pts_us;
                P.frames[slot].index  = frame_index;
                __atomic_store_n(&P.frames[slot].state, SLOT_READY,
                                 __ATOMIC_RELEASE);
            }

            frame_index++;
        }

        memmove(es, es + au, fill - au);
        fill -= au;
        annexb_reset(&st);
    }

    decoder_close();
    if (P.underruns)
        vs_log("%ld audio underruns (network could not keep up)", P.underruns);
    if (P.frames_dropped)
        vs_log("playback ended, %ld frames dropped (network or decode too "
               "slow -- try a lower resolution source)", P.frames_dropped);
    P.running = 0;
    return 0;
}

/* ---------------------------------------------------------------- public */

int vs_play_start(const char *ip, int port, const char *streamkey, int seek_s)
{
    /* Reclaim anything a previous session left behind BEFORE wiping the
     * struct that points at it. A start without a matching stop would
     * otherwise memset live texture and thread handles into oblivion --
     * leaking GPU memory that can never be freed, which after a few stream
     * switches is exactly the kind of exhaustion that faults the GPU. */
    if (P.frames[0].tex || P.athread > 0 || P.vdec_lib_init)
        vs_play_stop();

    /* One-time; a no-op on every call after the first. */
    if (pool_alloc() < 0) return -1;
    pool_blank();       /* no stale frame can flash before the first decode */

    memset(&P, 0, sizeof(P));
    P.ip   = ip;
    P.port = port;
    P.frame_memblk  = -1;
    P.net_fd[0]     = -1;
    P.net_fd[1]     = -1;
    P.fps_milli     = FPS * 1000;      /* replaced by the stream header */
    P.clock_epoch   = 0;       /* phase-locked clock re-seeds each playback */
    P.clock_locked  = 0;
    P.clock_samples = 0;

    snprintf(P.vpath, sizeof(P.vpath), "/v?s=%s&t=%d", streamkey, seek_s);
    {
        char extra[128];
        /* Both optional and both server-side no-ops when absent, so an
         * older middleware simply ignores them. */
        snprintf(extra, sizeof(extra), "%s%s%s",
                 g_boost ? "&b=1" : "",
                 g_content_id[0] ? "&id=" : "",
                 g_content_id[0] ? g_content_id : "");

        if (g_audio_track >= 0)
            snprintf(P.apath, sizeof(P.apath), "/a?s=%s&t=%d&a=%d%s",
                     streamkey, seek_s, g_audio_track, extra);
        else
            snprintf(P.apath, sizeof(P.apath), "/a?s=%s&t=%d%s",
                     streamkey, seek_s, extra);
    }

    P.cur_slot     = -1;
    P.pending_free = -1;
    P.buffering    = 0;
    P.stall_started = 0;
    P.av_trim_us   = g_saved_trim_us;   /* survives seeks and title changes */
    P.hz_micro    = 60000000;  /* starting guess; the estimator corrects it */
    P.vcount_base = 0;
    P.rate_n0     = 0;
    P.rate_window = RATE_WINDOW_START;

    /* Borrowed from the pool. Never allocated or freed per session, so the
     * GPU MMU is never touched during a stream change and no texture can be
     * released while the GPU still references it. */
    for (int i = 0; i < NUM_FRAMES; i++) {
        P.frames[i].tex   = g_pool_tex[i];
        P.frames[i].state = SLOT_FREE;
    }

    vs_ring_attach(&P.ring_v, g_pool_ring_v, RING_V);
    vs_ring_attach(&P.ring_a, g_pool_ring_a, RING_A);

    P.running = 1;

    /* All four threads use SCE_KERNEL_DEFAULT_PRIORITY (0x10000100).
     *
     * An earlier version tried to raise the network readers with values like
     * 0x10000090. That is not how this works: 0x10000100 is a magic constant
     * meaning "default", not a base to offset from, so those values were
     * rejected, thread creation failed, and playback refused to start. The
     * ring buffers are what actually absorb jitter; priority tuning was never
     * load-bearing, so it stays reverted. */
    P.vnet    = sceKernelCreateThread("vs_vnet", vnet_thread,
                                      0x10000100, 0x10000, 0, 0, NULL);
    P.anet    = sceKernelCreateThread("vs_anet", anet_thread,
                                      0x10000100, 0x10000, 0, 0, NULL);
    P.athread = sceKernelCreateThread("vs_audio", audio_thread,
                                      0x10000100, 0x10000, 0, 0, NULL);
    P.vthread = sceKernelCreateThread("vs_video", video_thread,
                                      0x10000100, 0x40000, 0, 0, NULL);

    /* Report which one failed -- "playback failed to start" with no detail
     * cost real debugging time. */
    if (P.vnet < 0 || P.anet < 0 || P.athread < 0 || P.vthread < 0) {
        vs_log("thread create failed: vnet=0x%08X anet=0x%08X "
               "audio=0x%08X video=0x%08X",
               P.vnet, P.anet, P.athread, P.vthread);
        P.running = 0;
        return -1;
    }

    sceKernelStartThread(P.vnet, 0, NULL);
    sceKernelStartThread(P.anet, 0, NULL);
    sceKernelStartThread(P.athread, 0, NULL);
    sceKernelStartThread(P.vthread, 0, NULL);
    return 0;
}

/* Called once per vblank from the main loop, before drawing.
 *
 * This is the whole point of the redesign: the decision of which frame is on
 * screen is made on the refresh boundary, so a 23.976fps stream settles into
 * a steady 3,2,3,2 cadence instead of choosing 2 or 3 at random depending on
 * when a background thread happened to flip a pointer.
 */
/* Which refresh should frame n appear on, relative to the anchor.
 * floor(n * 60 / fps). For 23.976 this yields 0,2,5,7,10,... whose deltas
 * are a steady 2,3,2,3 -- the correct pulldown for 24p on a 60Hz panel. */
static long refresh_due(long n)
{
    /* Micro-Hz, not milli-Hz: at 60Hz an integer milli-Hz step is ~17ppm, so
     * any correction finer than that truncated to zero and the estimate
     * parked on a lattice point up to 17ppm from truth. */
    return (long)((n * (long long)P.hz_micro)
                  / ((long long)P.fps_milli * 1000LL));
}

/* Newest ready frame that is due (index <= target) and not already shown.
 *
 * Matching the target index exactly would stall for 30 refreshes whenever an
 * AU produced no picture -- normal at stream start while the decoder fills
 * its pipeline -- and then resync. Taking the newest due frame keeps the
 * schedule while tolerating gaps. */
static int find_due(long target)
{
    int best = -1;

    for (int i = 0; i < NUM_FRAMES; i++) {
        if (__atomic_load_n(&P.frames[i].state, __ATOMIC_ACQUIRE) != SLOT_READY)
            continue;
        if (P.frames[i].index > target || P.frames[i].index <= P.shown_index)
            continue;
        if (best < 0 || P.frames[i].index > P.frames[best].index)
            best = i;
    }
    return best;
}

static void release_older_than(long index)
{
    for (int i = 0; i < NUM_FRAMES; i++) {
        /* Skip both the displayed slot and the one awaiting deferred
         * release -- the GPU may still be reading either. */
        if (i == P.cur_slot || i == P.pending_free) continue;
        if (__atomic_load_n(&P.frames[i].state, __ATOMIC_ACQUIRE) != SLOT_READY)
            continue;
        if (P.frames[i].index < index) {
            __atomic_store_n(&P.frames[i].state, SLOT_FREE, __ATOMIC_RELEASE);
            P.frames_dropped++;
        }
    }
}

static void switch_to(int slot)
{
    /* Release the slot displayed TWO switches ago, not the one we are
     * leaving now.
     *
     * vita2d_draw_texture only queues work; the GPU may still be reading the
     * outgoing texture when this runs. Freeing it immediately let the
     * decoder claim the slot and start writing into memory being scanned
     * out -- visible as torn pixels across the top of the frame, worst on
     * scene changes where the larger I-frames take longest to decode.
     *
     * One switch of deferral guarantees a full presented frame has elapsed,
     * which is comfortably longer than the GPU needs. It costs one slot: the
     * decoder still has two of four to work ahead in. */
    if (P.pending_free >= 0 && P.pending_free != slot)
        __atomic_store_n(&P.frames[P.pending_free].state, SLOT_FREE,
                         __ATOMIC_RELEASE);
    P.pending_free = (P.cur_slot != slot) ? P.cur_slot : -1;

    /* Record how many refreshes the outgoing frame was held. A healthy
     * 23.976 stream fills only the 2 and 3 buckets; anything in 1, 4 or 5
     * is the irregularity that reads as stutter.
     *
     * Starts at 1, not 0: the refresh on which a frame first appears is one
     * refresh of display time. */
    if (P.hold > 0)
        P.hold_hist[P.hold < 5 ? P.hold : 5]++;
    P.hold = 1;

    P.cur_slot           = slot;
    P.shown_index        = P.frames[slot].index;
    P.last_present_vsync = P.vsync_n;
    P.presented++;
}

/* Rate estimator, closed on the audio clock.
 *
 * An earlier version measured the panel's refresh rate directly and fed it
 * into the schedule. That was the wrong quantity to measure: it depends on
 * the loop running exactly once per vblank and on the process timer being a
 * true wall clock, and when either assumption slipped the measurement came
 * out high (60.12Hz on a 59.94Hz panel) and made playback drift worse than
 * the original hardcoded guess.
 *
 * What actually matters is that video does not drift against audio, and that
 * is directly observable: watch how the A/V offset changes over a window and
 * scale the rate by the same proportion. This nulls the drift whatever the
 * panel really does and however inaccurate the timers are, because
 * everything is referenced to the clock we are syncing to.
 *
 * Together with the existing offset correction this is a PI controller: the
 * anchor step handles position, this handles rate.
 */

/* Restart the rate window without touching the current estimate.
 *
 * Called wherever the A/V offset moves discontinuously. The estimate itself
 * stays -- it is the accumulated measurement that is invalidated, not the
 * conclusion drawn from earlier ones. */
static void rate_reset(void)
{
    P.rate_n0     = 0;
    P.rate_window = RATE_WINDOW_START;
}


static void track_rate(void)
{
    long elapsed, delta_av, ppm, step;

    if (!P.av_valid) return;                 /* offset filter not seeded yet */

    if (P.rate_n0 == 0) {
        P.rate_n0  = P.vsync_n;
        P.rate_av0 = P.av_offset_us;
        return;
    }

    elapsed = P.vsync_n - P.rate_n0;
    if (elapsed < P.rate_window) return;

    /* How far the offset moved across the window, as a fraction of the
     * window's own duration. Video losing 1ms per second is -1000 ppm.
     *
     * The window length comes from the current rate estimate rather than a
     * timer, which keeps the whole calculation referenced to the audio
     * clock and independent of how accurate the process timer is. */
    {
        long window_us = (long)((elapsed * 1000000000000LL) / P.hz_micro);
        if (window_us <= 0) return;
        delta_av = P.av_offset_us - P.rate_av0;

        /* Reject discontinuities.
         *
         * This measures how far the offset DRIFTED, which is only
         * meaningful if it moved gradually. A buffering stall, a backstop
         * present or any re-anchor moves it in one step, and reading that
         * step as a slope produces a wildly wrong rate -- logs showed
         * "drift 59999 ms/window" against a 60s window, i.e. the estimator
         * concluding the clocks run at completely different speeds.
         *
         * A real display is within a few hundred ppm of nominal, so a
         * genuine window can never move the offset by an appreciable
         * fraction of its own length. Anything that big is a jump: throw
         * the sample away and start a fresh window from here. */
        if (delta_av > window_us / 8 || delta_av < -window_us / 8) {
            rate_reset();
            return;
        }

        ppm = (delta_av * 1000000L) / window_us;
    }

    if (ppm >  RATE_MAX_STEP_PPM) ppm =  RATE_MAX_STEP_PPM;
    if (ppm < -RATE_MAX_STEP_PPM) ppm = -RATE_MAX_STEP_PPM;

    /* Damped to 60%: applying the full computed correction overshoots and
     * the estimate rings instead of settling. */
    step = (long)(((long long)P.hz_micro * ppm * 6) / 10000000LL);

    if (step != 0) {
        long old_hz = P.hz_micro;
        P.hz_micro += step;
        if (P.hz_micro < 50000000) P.hz_micro = 50000000;
        if (P.hz_micro > 70000000) P.hz_micro = 70000000;

        /* Rebase so the frame on screen stays put; a rate change must not
         * cause a visible jump. */
        P.anchor = P.vsync_n - refresh_due(P.shown_index);

        if (old_hz != P.hz_micro)
            vs_log("rate %ld.%04ld -> %ld.%04ld Hz (drift %ld ms/window)",
                   old_hz / 1000000, (old_hz % 1000000) / 100,
                   P.hz_micro / 1000000, (P.hz_micro % 1000000) / 100,
                   delta_av / 1000);
    }

    P.rate_n0  = P.vsync_n;
    P.rate_av0 = P.av_offset_us;

    P.rate_window *= 2;
    if (P.rate_window > RATE_WINDOW_MAX) P.rate_window = RATE_WINDOW_MAX;
}

/* Called once per vblank from the main loop, before drawing. */
void vs_play_present(void)
{
    long v_rel, target, drift;
    int  slot;

    /* Refresh counter, advanced by DELTAS rather than read as a position.
     *
     * The hardware vblank counter is preferred: if the main loop runs twice
     * inside one refresh, or misses one, a loop counter silently
     * desynchronises from the display and inflates any rate measured from
     * it. But on this hardware it has been seen to stop advancing entirely
     * while the app keeps rendering, and the whole schedule is differences
     * against this number -- so a frozen counter means refresh_due(next) is
     * never reached and the picture holds forever.
     *
     * The fix is in the shape of the code, not in a special case. vsync_n
     * is never ASSIGNED from a source; it is only ever advanced by however
     * much a source says has elapsed since the last look. That makes it
     * monotonic by construction, and it makes switching between sources
     * free -- there is no base to rebase and no reading to accept or
     * reject.
     *
     * That distinction is the whole bug from before. An earlier version
     * hand-incremented vsync_n during a stall and then compared raw counter
     * readings against the inflated value, rejecting every one as stale.
     * The count froze permanently and the picture froze with it. With
     * deltas there is nothing to compare and nothing to reject, so the same
     * mistake cannot be made twice.
     *
     * Falling back is also reversible. When the counter starts moving
     * again, its deltas are simply used again from that point -- no jump,
     * because position was never the thing being read. So a transient stall
     * costs a moment of timer-paced video and then true vblank alignment
     * comes back on its own. */
    {
        SceUInt64 wall = sceKernelGetProcessTimeWide();
        long      prev = P.vsync_n;
        int       vc   = sceDisplayGetVcount();
        long      step = 0;
        int       from_timer = 0;

        if (!P.tick_last_us) P.tick_last_us = wall;

        if (vc > 0 && P.vcount_last && vc != P.vcount_last) {
            long d = (long)(vc - P.vcount_last);
            /* Negative means the counter wrapped; positive but enormous
             * means it jumped. Neither is a reason to move the schedule by
             * that much, so resynchronise silently and let the timer cover
             * this tick. */
            if (d > 0 && d < 600) step = d;
            P.vcount_last    = vc;
            P.vcount_last_us = wall;
            if (P.counter_cold) {
                vs_log("vblank counter recovered; back to display pacing");
                P.counter_cold = 0;
            }
        } else if (vc > 0 && !P.vcount_last) {
            P.vcount_last    = vc;          /* first look, no delta yet */
            P.vcount_last_us = wall;
        }

        if (step == 0 && (wall - P.vcount_last_us) > 100000) {
            /* Counter has not moved for 100ms of real time. Pace from the
             * clock, which cannot stall, and keep the leftover fraction so
             * repeated small steps do not round away to nothing. */
            long long tick_us = 1000000000000LL / (P.hz_micro ? P.hz_micro
                                                              : 60000000);
            long long gap     = (long long)(wall - P.tick_last_us);

            if (tick_us > 0 && gap >= tick_us) {
                step = (long)(gap / tick_us);
                /* Advance by exactly what was consumed, NOT to wall: the
                 * remainder is the fraction of a refresh not yet elapsed,
                 * and discarding it each tick would lose time steadily. */
                P.tick_last_us += (SceUInt64)(step * tick_us);
                from_timer      = 1;
            }
            if (!P.counter_cold) {
                vs_log("vblank counter stalled at %d; pacing from the system "
                       "clock until it recovers", vc);
                P.counter_cold = 1;
            }
        }

        if (step > 0) {
            P.vsync_n += step;
            /* The timer branch already advanced its own accumulator by the
             * exact amount consumed; resetting it to wall here would throw
             * away the remainder it deliberately kept. */
            if (!from_timer) P.tick_last_us = wall;
        }

        if (P.paused) {
            /* Advance the anchor in step, so v_rel -- and therefore the
             * frame the schedule wants -- does not move. Without this,
             * resuming would find the schedule minutes ahead and race. */
            P.anchor += P.vsync_n - prev;
            P.hold++;
            P.repeats++;
            return;                      /* hold the current frame */
        }
    }
    track_rate();

    /* Bootstrap: align the vsync schedule to the audio clock.
     *
     * Anchoring to "whatever refresh the first frame happened to be ready
     * on" bakes in whatever A/V offset existed at that instant, and since
     * the schedule then free-runs off the vsync counter, that offset never
     * goes away. Anchor against the audio clock instead. */
    if (P.cur_slot < 0) {
        int  best = -1;
        long now, lead_refreshes;

        /* Wait for audio to be sounding AND for the clock filter to settle.
         *
         * Anchoring against the pre-lock fallback reading bakes in whatever
         * the startup transient happened to be, and since the schedule then
         * free-runs, that error persists for the whole segment. After a
         * seek this happens on every jump, which is why sync drifted a
         * little differently each time. Half a second of holding costs
         * nothing next to getting the anchor right. */
        if (P.samples_played == 0 || !P.clock_locked) { P.repeats++; return; }

        for (int i = 0; i < NUM_FRAMES; i++) {
            if (__atomic_load_n(&P.frames[i].state,
                                __ATOMIC_ACQUIRE) != SLOT_READY)
                continue;
            if (best < 0 || P.frames[i].index < P.frames[best].index)
                best = i;
        }
        if (best < 0) { P.repeats++; return; }

        /* This frame should be on screen when the audio clock reads its pts.
         * If the clock is already past that, we are late by the difference,
         * so shift the anchor back by that many refreshes. */
        now = audio_clock_us();
        {
            long period_us = (long)(1000000000000LL / P.hz_micro);
            long err       = now - P.frames[best].pts_us;
            lead_refreshes = (err >= 0)
                             ? (err + period_us / 2) / period_us
                             : (err - period_us / 2) / period_us;
        }

        P.anchor = P.vsync_n - refresh_due(P.frames[best].index)
                   - lead_refreshes;

        P.av_valid = 0;                    /* filter must re-seed after a jump */
        rate_reset();                      /* and the window must not span it */
        vs_log("A/V anchored: frame %ld pts %ld, clock %ld, offset %ld ms",
               P.frames[best].index, P.frames[best].pts_us, now,
               (now - P.frames[best].pts_us) / 1000);
        switch_to(best);
        return;
    }

    v_rel = P.vsync_n - P.anchor;

    /* Advance to the newest frame whose scheduled refresh has arrived.
     * Walking forward from the current index keeps this exact regardless of
     * the flooring in refresh_due. */
    target = P.shown_index;
    while (refresh_due(target + 1) <= v_rel)
        target++;

    if (target == P.shown_index) {
        P.repeats++;
        P.hold++;

        /* BACKSTOP.
         *
         * Everything above this point is the vsync schedule, which exists
         * to get 3:2 cadence right for 23.976 on a 60Hz panel. It is worth
         * having, but it is built on differences against a hardware counter
         * and an anchor, and every freeze in this player so far has been
         * that arithmetic reaching a state it cannot leave.
         *
         * This is the escape that does not use any of it. If the audio
         * clock -- monotonic, derived from the system timer, and the thing
         * the viewer actually hears -- is already past a decoded frame's
         * timestamp by more than BACKSTOP_LATE_US, that frame is late and
         * belongs on screen now. Show it. No anchor, no refresh_due, no
         * counter.
         *
         * It cannot deadlock, because the condition depends only on a clock
         * that always advances and on frames that are already decoded. If
         * the schedule is healthy this never fires: frames are presented
         * before they are ever 250ms late. If the schedule is stuck, this
         * keeps the picture moving in sync with the sound regardless of
         * why. */
        if (P.clock_locked && P.hold > 8) {
            long now  = audio_clock_us();
            int  late = -1;

            for (int i = 0; i < NUM_FRAMES; i++) {
                if (__atomic_load_n(&P.frames[i].state,
                                    __ATOMIC_ACQUIRE) != SLOT_READY)
                    continue;
                if (P.frames[i].index <= P.shown_index)
                    continue;
                if (now - P.frames[i].pts_us < BACKSTOP_LATE_US)
                    continue;
                /* Newest frame that is genuinely due, so a stuck schedule
                 * catches up in one step rather than crawling. */
                if (late < 0 || P.frames[i].index > P.frames[late].index)
                    late = i;
            }

            if (late >= 0) {
                if (!P.backstop_fired) {
                    vs_log("backstop: schedule not advancing, presenting "
                           "frame %ld directly (%ld ms late)",
                           P.frames[late].index,
                           (now - P.frames[late].pts_us) / 1000);
                    P.backstop_fired = 1;
                }
                /* Re-anchor so the normal schedule resumes from here
                 * instead of leaving this to fire on every frame. */
                P.anchor = P.vsync_n - refresh_due(P.frames[late].index);
                P.av_valid = 0;
                rate_reset();
                release_older_than(P.frames[late].index);
                switch_to(late);
                return;
            }
        }

        /* DIAGNOSTIC.
         *
         * A hold is normal -- it is how a 23.976 stream maps onto 60Hz. A
         * hold lasting seconds is not, and the existing "stalled" log
         * cannot see it: that only fires once the schedule WANTS a new
         * frame, and this branch is the case where it does not.
         *
         * If this line appears during a freeze, the schedule is advancing
         * its anchor as fast as the vsync counter, and paused/anchor/v_rel
         * say which of the two ways that can happen is responsible. If it
         * does NOT appear, vs_play_present is not being called at all and
         * the fault is in the main loop, not here. */
        if (P.hold > 180 && (P.hold % 180) == 0)
            vs_log("holding %d refreshes: paused=%d vsync=%ld anchor=%ld "
                   "v_rel=%ld shown=%ld due_next=%ld hz=%ld",
                   P.hold, P.paused, P.vsync_n, P.anchor, v_rel,
                   P.shown_index, refresh_due(P.shown_index + 1),
                   P.hz_micro);

        /* WATCHDOG.
         *
         * A hold is how 23.976 maps onto 60Hz, and a legitimate one lasts
         * two or three refreshes. Two seconds of holding while decoded
         * frames sit READY and unshown is not a cadence, it is a stuck
         * schedule: the decoder has filled every slot it owns and is
         * blocked, so nothing will ever free one. That is the freeze --
         * picture stopped, sound continuing on its own thread, UI still
         * responsive because the main loop is fine.
         *
         * v_rel comes from sceDisplayGetVcount(), so if that counter stops
         * advancing the schedule can never reach the next frame no matter
         * how long it waits. Rather than trying to distinguish the reasons
         * it might stop, re-anchor to the newest decoded frame and carry
         * on. This is the same recovery the "schedule is fiction" path
         * below already performs, applied to the case where the schedule
         * has stopped moving instead of running ahead.
         *
         * Gated on a READY frame existing so it can never fire during a
         * legitimate wait for data -- with nothing decoded there is nothing
         * to re-anchor to, and holding is the correct behaviour. */
        if (P.hold > 120) {
            int newest = -1;
            for (int i = 0; i < NUM_FRAMES; i++) {
                if (__atomic_load_n(&P.frames[i].state,
                                    __ATOMIC_ACQUIRE) != SLOT_READY)
                    continue;
                if (newest < 0 || P.frames[i].index > P.frames[newest].index)
                    newest = i;
            }
            if (newest >= 0 && P.vsync_n - P.last_watchdog_vsync > 300) {
                /* Re-anchor against the AUDIO CLOCK, not just to whatever
                 * was decoded last.
                 *
                 * Anchoring blindly gets the schedule moving again but
                 * places the picture wherever the decoder happened to have
                 * reached, which can be a second or two from the sound.
                 * Drift correction then has to recover that at one refresh
                 * per second -- around 17ms/s -- so a two second error
                 * takes two minutes to close, and looks to a viewer like
                 * sync is simply broken.
                 *
                 * Same arithmetic as the bootstrap anchor: work out how
                 * many refreshes this frame is late by against the clock,
                 * and offset the anchor by that. */
                long now  = audio_clock_us();
                long per  = (long)(1000000000000LL / P.hz_micro);
                long err  = now - P.frames[newest].pts_us;
                long lead = (err >= 0) ? (err + per / 2) / per
                                       : (err - per / 2) / per;

                /* CLAMP. Compensating for a large deficit moves the
                 * schedule past everything that has been decoded, so
                 * find_due fails, the "schedule is fiction" path below
                 * re-anchors WITHOUT compensation, and this fires again --
                 * the two recoveries undo each other roughly every two
                 * seconds. Seen as video racing, stalling, racing again
                 * while the audio stays fine.
                 *
                 * Past this point the problem is not the schedule. Video
                 * that is seconds behind is video that was never delivered,
                 * and no anchor arithmetic conjures frames that do not
                 * exist. Correct what is correctable, say so plainly, and
                 * leave the rest to drift correction. */
                if (lead >  WATCHDOG_MAX_LEAD) lead =  WATCHDOG_MAX_LEAD;
                if (lead < -WATCHDOG_MAX_LEAD) lead = -WATCHDOG_MAX_LEAD;

                if (err > 1000000 || err < -1000000)
                    /* err = clock - pts. POSITIVE means the frame is older
                     * than the clock, i.e. video is behind. Negative means
                     * the frame is from the future and the AUDIO is behind
                     * -- the opposite problem, and reporting both as
                     * "behind audio" made a stalling audio stream look like
                     * a slow video pipeline. */
                    vs_log("LARGE A/V GAP: picture %ld ms %s sound at frame "
                           "%ld. Too large for the schedule to absorb -- "
                           "%s is not keeping up.",
                           (err > 0 ? err : -err) / 1000,
                           err > 0 ? "behind" : "ahead of",
                           P.frames[newest].index,
                           err > 0 ? "video (source, network or encoder)"
                                   : "audio (network)");
                else
                    vs_log("schedule stuck at v_rel=%ld after %d refreshes; "
                           "re-anchoring to frame %ld (%ld ms from audio)",
                           v_rel, P.hold, P.frames[newest].index, err / 1000);

                P.last_watchdog_vsync = P.vsync_n;
                P.anchor = P.vsync_n - refresh_due(P.frames[newest].index)
                           - lead;
                P.av_valid = 0;         /* the filter must re-seed after a jump */
                rate_reset();
                release_older_than(P.frames[newest].index);
                switch_to(newest);      /* resets P.hold */
                P.resyncs++;
            }
        }
        return;                       /* correct hold, not a stall */
    }

    /* If the schedule wants a new frame but nothing has been presented for
     * several seconds, record what the pipeline looks like. A freeze that
     * leaves no evidence is very expensive to chase; this costs one log
     * line. */
    if (P.vsync_n - P.last_present_vsync > 300 && P.last_present_vsync) {
        int ready = 0;
        for (int i = 0; i < NUM_FRAMES; i++)
            if (__atomic_load_n(&P.frames[i].state,
                                __ATOMIC_ACQUIRE) == SLOT_READY) ready++;
        vs_log("stalled %lds: ring_v=%d ring_a=%d ready=%d "
               "veof=%d aeof=%d shown=%ld target=%ld",
               (P.vsync_n - P.last_present_vsync) / 60,
               vs_ring_used(&P.ring_v), vs_ring_used(&P.ring_a), ready,
               vs_ring_eof(&P.ring_v), vs_ring_eof(&P.ring_a),
               P.shown_index, target);
        P.last_present_vsync = P.vsync_n;   /* rate-limit to once per 5s */
    }

    slot = find_due(target);
    if (slot < 0) {
        /* The scheduled frame has not been decoded yet. Hold rather than
         * showing something else -- substituting a neighbour is precisely
         * the irregular cadence this design exists to avoid. */
        P.repeats++;
        P.hold++;

        /* Unless we have fallen so far behind that the schedule is fiction,
         * in which case re-anchor to whatever is actually available. */
        if (P.hold > 30) {
            int newest = -1;
            for (int i = 0; i < NUM_FRAMES; i++) {
                if (__atomic_load_n(&P.frames[i].state,
                                    __ATOMIC_ACQUIRE) != SLOT_READY)
                    continue;
                if (newest < 0 || P.frames[i].index > P.frames[newest].index)
                    newest = i;
            }
            if (newest >= 0) {
                P.anchor = P.vsync_n - refresh_due(P.frames[newest].index);
                release_older_than(P.frames[newest].index);
                switch_to(newest);
                P.resyncs++;
            }
        }
        return;
    }

    release_older_than(P.frames[slot].index);
    switch_to(slot);

    /* Drift correction. The display and audio clocks run off separate
     * dividers, so they separate over time no matter how good the initial
     * alignment is.
     *
     * The old 250ms threshold was chosen to avoid disturbing the cadence,
     * but 250ms of lip-sync error is grossly audible -- roughly five times
     * the point where a viewer notices sound trailing picture. Correct from
     * 45ms instead, one refresh (16.7ms) at a time, no more than once a
     * second. That is slow enough that the cadence stays regular while still
     * pulling A/V back together within a few seconds. */
    {
        long raw = P.frames[slot].pts_us - audio_clock_us();

        if (!P.av_valid) {                 /* seed, don't ramp from zero */
            P.av_offset_us = raw;
            P.av_valid = 1;
        } else {
            /* Division, not >>: an arithmetic right shift rounds toward
             * negative infinity, so a negative error term would round away
             * from zero and bias the filter when video runs behind audio.
             * Division truncates toward zero, which is symmetric. */
            P.av_offset_us += (raw - P.av_offset_us) / AV_EMA_DIV;
        }
        /* Correct toward the user's trim rather than toward zero.
         *
         * The residual constant offset here is dominated by how deeply the
         * audio hardware buffers ahead of what the clock reports, which is
         * not something this code can measure. A listener can hear it, so
         * expose it rather than guess a compensation value. */
        drift = P.av_offset_us - P.av_trim_us;
    }

    /* Nothing to correct while buffering.
     *
     * The clock is deliberately held still across a stall, so the offset
     * against it grows for as long as the stall lasts -- not because
     * anything is wrong, but because measured time has stopped while frames
     * keep arriving. Correcting on that reading chases an error that is
     * about to vanish on its own the moment the clock restarts. */
    if (P.buffering) return;

    /* One refresh per correction, at most once per second, while the error
     * is small: slow enough that the cadence stays regular, fast enough to
     * close a visible gap in a few seconds. Past AV_SNAP_US it corrects the
     * whole error at once instead -- see below. */
    if ((drift > AV_CORRECT_US || drift < -AV_CORRECT_US) &&
        P.vsync_n - P.last_correct_vsync > 60) {
        long period_us = (long)(1000000000000LL / P.hz_micro);
        long steps     = 1;
        int  big       = (drift > AV_SNAP_US || drift < -AV_SNAP_US);

        /* Correct in PROPORTION to the error, not one refresh at a time.
         *
         * A single refresh per second is 16.7ms/s of authority -- enough for
         * the tens of milliseconds this loop was built for, and nothing
         * larger. After two seconds of buffering the offset is hundreds of
         * milliseconds, which at that rate takes half a minute, and if the
         * rate estimate is even slightly off it grows faster than the
         * correction removes it.
         *
         * A snap takes the whole error out in one move instead. But ONE
         * move: a snap needs seconds to actually play out, because it works
         * by moving the schedule and letting the refresh count catch up.
         * Re-measuring a second later shows the error still there, snapping
         * again, and again -- which does not correct anything, it just
         * re-presents once a second. That is 1fps video with the sound
         * running normally, and it is exactly what happened before this
         * cooldown existed. */
        if (big && P.vsync_n - P.last_snap_vsync < AV_SNAP_COOLDOWN)
            return;

        if (big) {
            long mag = (drift > 0) ? drift : -drift;
            steps = (mag + period_us / 2) / period_us;
            if (steps < 1) steps = 1;
            vs_log("sync snap: picture %ld ms %s sound, correcting %ld "
                   "refreshes at once", mag / 1000,
                   drift > 0 ? "ahead of" : "behind", steps);
            P.last_snap_vsync = P.vsync_n;
            /* A jump this size must not be averaged across by the filter,
             * and the baseline shift below cannot represent it honestly
             * either -- so re-seed both rather than distort them. */
            P.av_valid = 0;
            rate_reset();
        }

        P.anchor += (drift > 0) ? steps : -steps;
        P.last_correct_vsync = P.vsync_n;
        P.resyncs++;

        /* Shift BOTH the filter and the rate baseline by the same amount.
         *
         * The jump is our own doing, not drift, so it must not appear in the
         * estimator's delta. Shifting the baseline cancels it exactly while
         * keeping the measurement window running -- restarting the window
         * instead (what this did before) capped the baseline at the gap
         * between corrections, and slope accuracy scales with baseline
         * length. */
        P.av_offset_us += (drift > 0) ? -period_us * steps : period_us * steps;
        P.rate_av0     += (drift > 0) ? -period_us * steps : period_us * steps;
    }
}

void vs_play_draw(void)
{
    int c = P.cur_slot;
    if (c >= 0 && P.frames[c].tex)
        vita2d_draw_texture(P.frames[c].tex, 0, 0);
}

void vs_play_set_paused(int paused)
{
    if (paused == P.paused) return;

    if (paused) {
        P.pause_started = sceKernelGetProcessTimeWide();
    } else if (P.pause_started) {
        P.pause_accum_us += (long)(sceKernelGetProcessTimeWide()
                                   - P.pause_started);
        P.pause_started = 0;
    }
    P.paused = paused;
    vs_log("%s", paused ? "paused" : "resumed");
}

int vs_play_is_paused(void) { return P.paused; }

long vs_play_duration_s(void) { return P.duration_s; }

/* Nudge the sync target. Positive moves picture later relative to sound. */
void vs_play_trim(int delta_us)
{
    long t = P.av_trim_us + delta_us;

    /* Wide clamp on purpose.
     *
     * The previous +/-120ms limit was set assuming the residual would be
     * small hardware latency. If the real skew is stream-level -- the video
     * and audio ffmpeg processes starting at slightly different points --
     * it can be far larger, and a tight clamp silently pins the trim at the
     * limit where it looks like an optimum. Only reject values that are
     * clearly nonsense. */
    if (t >  500000) t =  500000;
    if (t < -500000) t = -500000;
    P.av_trim_us    = t;
    g_saved_trim_us = t;

    /* Clear the correction cooldown so the change can take effect on the
     * next frame instead of waiting out the one-second interval. Without
     * this, small adjustments feel unresponsive and it is easy to overshoot
     * while hunting for the right value. */
    P.last_correct_vsync = P.vsync_n - 61;

    vs_log("A/V trim set to %+ld ms", t / 1000);
}

void vs_play_stats(vs_play_stat *st)
{
    int q = 0;
    for (int i = 0; i < NUM_FRAMES; i++)
        if (__atomic_load_n(&P.frames[i].state, __ATOMIC_ACQUIRE) == SLOT_READY)
            q++;
    st->presented = P.presented;
    st->dropped   = P.frames_dropped;
    st->underruns = P.underruns;
    st->resyncs   = P.resyncs;
    st->queued    = q;
    /* Error from the target, not the raw offset. With a non-zero trim the
     * raw number sits at the trim value and looks alarming; what matters is
     * whether the controller is holding the target. */
    st->av_ms     = (int)((P.av_offset_us - P.av_trim_us) / 1000);
    st->hz_milli  = P.hz_micro / 1000;
    st->trim_ms   = (int)(P.av_trim_us / 1000);
    /* What is actually applied, so a large residual is visible rather than
     * hidden behind a built-in constant. */
    st->total_ms  = (int)((P.av_trim_us + AUD_PIPELINE_OFFSET_US) / 1000);
    for (int i = 0; i < 6; i++) st->hold_hist[i] = P.hold_hist[i];

    /* Irregular = frames held for anything other than 2 or 3 refreshes.
     * This is the number that matters: a cadence average of 2.50 can hide a
     * random 2/3 mix, and the randomness is what is visible as stutter. */
    st->irregular = P.hold_hist[1] + P.hold_hist[4] + P.hold_hist[5];
}

int vs_play_running(void) { return P.running; }

long vs_play_position_s(void) { return audio_clock_us() / 1000000; }

/* Safe to call twice, and safe after a failed vs_play_start.
 *
 * Thread handles are zero when start failed before creating them, and
 * waiting on or deleting handle 0 is not valid. Textures are freed only if
 * still owned, and nulled as they go. */
void vs_play_stop(void)
{
    SceUID th[4];
    int i;

    P.quit_requested = 1;
    P.running = 0;

    /* Close the reader sockets FIRST. Without this the join below waits out
     * a 20-second recv timeout on the main thread, which stalls the display
     * queue and takes the GPU down with it. */
    net_close(0);
    net_close(1);

    th[0] = P.athread; th[1] = P.vthread;
    th[2] = P.anet;    th[3] = P.vnet;

    /* Bounded join. With the sockets closed the threads exit in
     * milliseconds, but a timeout means no future blocking call can ever
     * wedge the main loop -- and stalling the main loop is what takes the
     * display queue, and therefore the GPU, down. Three seconds is far
     * beyond a healthy exit and far below the watchdog. */
    for (i = 0; i < 4; i++) {
        SceUInt32 timeout = 3 * 1000 * 1000;
        if (th[i] <= 0) continue;
        if (sceKernelWaitThreadEnd(th[i], NULL, &timeout) < 0) {
            vs_log("thread %d did not exit within 3s; leaking its handle "
                   "rather than risking a delete while it runs", i);
            th[i] = 0;              /* do not delete a thread still running */
        }
    }
    for (i = 0; i < 4; i++)
        if (th[i] > 0) sceKernelDeleteThread(th[i]);

    P.athread = P.vthread = P.anet = P.vnet = 0;

    /* Ring buffers are pool-owned; just detach. */
    P.ring_v.buf = NULL;
    P.ring_a.buf = NULL;

    /* Same hazard as the poster textures: the main thread may have queued a
     * draw of these on the frame we stopped, and freeing under the GPU is an
     * unrecoverable fault. Stop presenting first, then let the GPU drain. */
    P.cur_slot     = -1;
    P.pending_free = -1;
    vita2d_wait_rendering_done();

    /* Nothing is freed: the textures outlive the session. That removes, by
     * construction, every hazard around releasing memory the GPU may still
     * be reading. */
    for (int i = 0; i < NUM_FRAMES; i++) {
        P.frames[i].tex   = NULL;
        P.frames[i].state = SLOT_FREE;
    }
}
