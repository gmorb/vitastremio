#ifndef VS_PLAYER_H
#define VS_PLAYER_H

/* Start playback of a middleware stream key, seeking to seek_s seconds.
 * Spawns the audio and video threads and returns immediately. */
int  vs_play_start(const char *ip, int port, const char *streamkey, int seek_s);

/* Call once per vblank, BEFORE vita2d_start_drawing. Chooses which decoded
 * frame should be on screen this refresh. Keeping this on the vsync boundary
 * is what gives a regular cadence. */
void vs_play_present(void);

/* Call once per frame between vita2d_start_drawing/end_drawing. */
void vs_play_draw(void);

/* Playback counters for the on-screen overlay.
 *
 * hold_hist[n] counts frames displayed for exactly n refreshes (index 5 is
 * "5 or more"). On a 60Hz panel a healthy 23.976fps stream lands entirely in
 * buckets 2 and 3; 25fps lands in 2 and 3 as well; 30fps lands entirely in
 * bucket 2. Anything in 1, 4 or 5 is irregular cadence, which is what stutter
 * actually is -- and it is invisible in an average, since a random mix of 2s
 * and 3s averages the same 2.5 as a perfect alternation. */
typedef struct {
    long presented;
    long dropped;      /* decoded but never shown -- should stay 0 */
    long underruns;    /* audio starved -- should stay 0 */
    long resyncs;      /* schedule re-anchored after falling behind */
    int  av_ms;        /* error from the sync target, ms; should hover near 0 */
    long hz_milli;     /* measured panel refresh rate x1000 */
    int  trim_ms;      /* user A/V trim currently applied */
    int  total_ms;     /* trim + the built-in pipeline offset */
    long hold_hist[6];
    long irregular;    /* hold_hist[1] + [4] + [5] */
    int  queued;
} vs_play_stat;

void vs_play_stats(vs_play_stat *st);

/* Adjust the A/V sync target by delta_us. Positive moves picture later
 * relative to sound. Persists across seeks and titles. */
void vs_play_trim(int delta_us);

/* Nonzero while either stream is still alive. */
int  vs_play_running(void);

/* Nonzero while playback is held waiting for the audio ring to refill.
 *
 * The master clock is frozen for the duration, so video holds with the
 * sound rather than running ahead of it. Draw an indicator while this is
 * set -- otherwise a network stall looks like the player has desynced. */
int  vs_play_buffering(void);

/* Total length of the stream in seconds; 0 if the server didn't report it
 * (live sources, or a probe that failed). Callers must handle 0 rather than
 * drawing a progress bar that implies false progress. */
long vs_play_duration_s(void);

/* Release textures retired by vs_play_stop. Call once per frame from the
 * main loop, whether or not playback is running. */
void vs_play_gc(void);

/* Audio stream selection, -1 for the file's default. Takes effect on the
 * next vs_play_start, so switching means restarting at the current position
 * -- the audio clock is the master, and swapping it underneath a running
 * schedule would desync everything downstream of it. */
void vs_play_set_audio_track(int idx);
int  vs_play_audio_track(void);

/* Headphone/Bluetooth loudness boost. Film is mastered around -24 LUFS,
 * which is correct for a cinema and far too quiet for a handheld; on
 * headphones the Vita's speakers are no longer the limit, so the extra
 * gain is usable. The server applies it, behind a limiter that is always
 * in the chain -- the Vita cannot exceed 0 dB on its own output.
 *
 * Like track selection, this takes effect on the next vs_play_start. */
void vs_play_set_boost(int on);
int  vs_play_boost(void);

/* Content id (tt0111161, or tt0903747:2:5) for the title being played.
 *
 * Passed to the server on the audio request so it can record how far this
 * title has been watched. Keyed on content rather than on the stream key,
 * because the stream key names one particular source -- resume would
 * otherwise break the moment a different source was picked for the same
 * episode, which is routine when a debrid link expires.
 *
 * Optional: playback is unaffected if it is never set, only resume is. */
void vs_play_set_content_id(const char *id);

/* Pause and resume. Freezes the audio clock and the video schedule together,
 * so resuming continues from exactly where it stopped. While paused the
 * rings back up and the middleware's ffmpeg blocks on the socket, so a pause
 * costs nothing upstream either. */
void vs_play_set_paused(int paused);
int  vs_play_is_paused(void);

/* Elapsed seconds within the current segment, from the audio clock.
 * Add the seek offset yourself to get absolute position. */
long vs_play_position_s(void);

/* Tear down threads, decoder and textures. Safe to call after natural end. */
void vs_play_stop(void);

#endif
