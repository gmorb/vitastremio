/* main.c -- vitastremio browser.
 *
 * Screens: CATALOG -> STREAMS -> PLAYING
 *
 * The middleware hands us records separated by 0x0A with fields separated
 * by 0x1F, so "parsing" is two strtok loops and no JSON library.
 */

#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vita2d.h>

#include "http.h"
#include "lineproto.h"
#include "log.h"
#include "config.h"
#include "ime.h"
#include "player.h"

/* Fallback only. The real address is entered on the device with START and
 * persisted to ux0:data/vitastremio.cfg, so no rebuild is needed to move
 * the server. Editing this just saves typing on a fresh install. */
#define MW_IP_DEFAULT   "192.168.1.10"
#define MW_PORT_DEFAULT 8480

static char g_mw_ip[VS_IP_MAX] = MW_IP_DEFAULT;
static int  g_mw_port          = MW_PORT_DEFAULT;

#define MAX_ITEMS    240
#define MAX_STREAMS  60

#define GRID_COLS    5
#define GRID_ROWS    2
#define CELL_W       176
#define CELL_H       218
/* Centred: 4 gaps of CELL_W plus one poster is 832 wide, so 64 either side.
 * The old value left 28 on the left and 100 on the right. */
#define GRID_X       64
#define GRID_Y       70

#define POSTER_W     128
#define POSTER_H     186

/* Posters are released once they are well off screen. Nothing freed them
 * before, so paging through a 240-item catalog accumulated every texture it
 * had ever loaded -- around 22MB of GPU memory, on top of the 8.4MB the
 * player needs for frame buffers. Keeping a few pages either side is enough
 * that paging back and forth never refetches. */
#define POSTER_KEEP_PAGES   2

#define POSTER_MAX_TRIES    3
#define POSTER_RETRY_FRAMES 240        /* ~4s between attempts */

#define C_BG      RGBA8(0x0C, 0x0A, 0x14, 0xFF)
#define C_TEXT    RGBA8(0xF2, 0xF0, 0xF7, 0xFF)
#define C_DIM     RGBA8(0x9A, 0x94, 0xAD, 0xFF)
#define C_ACCENT  RGBA8(0x7B, 0x5C, 0xFF, 0xFF)
#define C_PANEL   RGBA8(0x1B, 0x17, 0x28, 0xFF)

/* Same colour with a different alpha. Everything in the transport bar fades
 * as a group, so alpha has to be a parameter rather than baked into the
 * constants. */
#define WITH_A(c, a) (((c) & 0x00FFFFFFu) | ((unsigned)(a) << 24))

/* ------------------------------------------------------------- ui bits
 *
 * vita2d draws rectangles, circles and lines. Everything below is built
 * from those three: rounded panels from a cross of rectangles plus corner
 * circles, gradients from stacked one-pixel rows, and icons from scanline
 * triangles. No textures, so nothing to load or free.
 */

typedef struct { int x, y, w, h; } rect;

static vita2d_pgf *g_font;

/* Raw held-button state, published by the main loop. Needed by handlers for
 * combos and auto-repeat, which edge-triggered input cannot express. */
static unsigned int pad_now;

/* ------------------------------------------------- primitive accounting
 *
 * vita2d packs every primitive in a frame into one vertex pool and does not
 * bounds-check it: past the end it keeps returning pointers and the GPU
 * reads whatever is there as vertex data. That faults the GPU with no
 * catchable error, which is what the crash dumps show.
 *
 * Counting draw calls turns "probably the pool" into a number that can be
 * read off the screen. The wrappers below are shadowed over the vita2d
 * names, so ordinary call sites need no changes.
 */
#define VITA2D_POOL_BYTES (4 * 1024 * 1024)

static long g_prim_rect, g_prim_circ, g_prim_line, g_prim_text;
static long g_prim_peak;

static void vs_rect(float x, float y, float w, float h, unsigned int c)
{
    g_prim_rect++;
    vita2d_draw_rectangle(x, y, w, h, c);
}

static void vs_circ(float x, float y, float r, unsigned int c)
{
    g_prim_circ++;
    vita2d_draw_fill_circle(x, y, r, c);
}

static void vs_line(float a, float b, float c2, float d, unsigned int c)
{
    g_prim_line++;
    vita2d_draw_line(a, b, c2, d, c);
}

static int vs_pgf(vita2d_pgf *f, int x, int y, unsigned int c, float sz,
                  const char *t)
{
    g_prim_text += (long)strlen(t);
    return vita2d_pgf_draw_text(f, x, y, c, sz, t);
}

/* Shadow the library names from here down. */
#define vita2d_draw_rectangle   vs_rect
#define vita2d_draw_fill_circle vs_circ
#define vita2d_draw_line        vs_line
#define vita2d_pgf_draw_text    vs_pgf

/* Rough pool cost. vita2d uses a position+colour vertex and 6 indices per
 * quad; circles are a fan of segments. Exact sizes are internal to vita2d,
 * so this is an estimate -- but a consistent one, which is what matters for
 * spotting a frame that balloons. */
#define EST_RECT_BYTES  76
#define EST_CIRC_BYTES  1800
#define EST_GLYPH_BYTES 76

static long prim_bytes(void)
{
    return g_prim_rect * EST_RECT_BYTES
         + g_prim_circ * EST_CIRC_BYTES
         + g_prim_line * EST_RECT_BYTES
         + g_prim_text * EST_GLYPH_BYTES;
}

/* PGF has no bold face, so weight is faked by drawing the same string twice
 * a pixel apart. Cheap, and the difference between this and single-pass
 * text is most of what made the UI read as thin. */
static void ui_text(float x, float y, unsigned int col, float sz,
                    const char *t)
{
    vita2d_pgf_draw_text(g_font, x, y, col, sz, t);
}

static void ui_text_b(float x, float y, unsigned int col, float sz,
                      const char *t)
{
    vita2d_pgf_draw_text(g_font, x,        y, col, sz, t);
    vita2d_pgf_draw_text(g_font, x + 1.0f, y, col, sz, t);
}

static void ui_round_rect(float x, float y, float w, float h, float r,
                          unsigned int col)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    vita2d_draw_rectangle(x + r, y,     w - 2 * r, h,         col);
    vita2d_draw_rectangle(x,     y + r, r,         h - 2 * r, col);
    vita2d_draw_rectangle(x + w - r, y + r, r,     h - 2 * r, col);
    vita2d_draw_fill_circle(x + r,     y + r,     r, col);
    vita2d_draw_fill_circle(x + w - r, y + r,     r, col);
    vita2d_draw_fill_circle(x + r,     y + h - r, r, col);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, col);
}

/* Vertical gradient, transparent at the top. Softens the bar into the
 * picture instead of cutting a hard band across it. */
static void ui_scrim(float y, float h, int top_a, int bot_a, unsigned int rgb)
{
    /* Bands that tile exactly: each starts where the previous ended and none
     * overlaps.
     *
     * An earlier version stepped by a fractional h/steps and drew each row
     * one pixel taller than its step, so consecutive rows overlapped. Alpha
     * blended twice in the overlap and darkened it, which appeared as faint
     * horizontal lines across the gradient.
     *
     * Two-pixel bands rather than one: at these alphas the difference is not
     * visible, and it halves the primitive count. That matters because
     * vita2d's per-frame vertex pool is finite and unchecked, and this UI
     * draws several hundred primitives a frame. */
    const int STEP = 2;
    int y0 = (int)y, rows = (int)h;

    if (rows < 1) return;
    for (int i = 0; i < rows; i += STEP) {
        int last = (rows - 1) ? rows - 1 : 1;
        int a    = top_a + (bot_a - top_a) * i / last;
        int hgt  = (i + STEP <= rows) ? STEP : rows - i;
        if (a <= 0) continue;
        vita2d_draw_rectangle(0, y0 + i, 960, hgt, WITH_A(rgb, a));
    }
}

/* Round off the corners of something already drawn -- a poster texture, in
 * practice -- by painting the area outside the arc in the background colour.
 *
 * vita2d cannot clip a texture to a rounded shape, and the alternative
 * (leaving square corners) is most of what makes a grid of artwork look
 * unfinished. Only valid over a solid known background, which the catalog
 * is. */
static void ui_mask_corners(float x, float y, float w, float h, float r,
                            unsigned int bg)
{
    /* Two-pixel steps rather than one.
     *
     * At this radius the difference is invisible, but it halves what was by
     * far the heaviest thing on screen: with ten posters the per-pixel
     * version issued 280 draw calls per frame just for corners. Primitive
     * count matters here because vita2d's vertex pool is finite and
     * unchecked. */
    const int step = 2;
    for (int i = 0; i < (int)r; i += step) {
        float dy = r - i;
        float dx = r - sqrtf(r * r - dy * dy);
        if (dx < 0.5f) continue;
        vita2d_draw_rectangle(x,          y + i,            dx, step, bg);
        vita2d_draw_rectangle(x + w - dx, y + i,            dx, step, bg);
        vita2d_draw_rectangle(x,          y + h - step - i, dx, step, bg);
        vita2d_draw_rectangle(x + w - dx, y + h - step - i, dx, step, bg);
    }
}

/* Scanline-filled triangle. dir +1 points right, -1 left, 2 up. */
static void ui_triangle(float cx, float cy, float size, int dir,
                        unsigned int col)
{
    int rows = (int)size;

    if (dir == 2) {                 /* pointing up */
        for (int i = 0; i <= rows; i++) {
            float w  = (float)i / rows * size * 0.9f;
            float ry = cy - size / 2.0f + i;
            if (w < 1.0f) continue;
            vita2d_draw_rectangle(cx - w / 2.0f, ry, w, 1, col);
        }
        return;
    }

    for (int i = 0; i <= rows; i++) {
        float t = rows ? (float)i / rows : 0.0f;
        float d = fabsf(t - 0.5f) * 2.0f;          /* 1 at tips, 0 at middle */
        float w = (1.0f - d) * size * 0.62f;
        float ry = cy - size / 2.0f + i;
        if (w < 1.0f) continue;
        if (dir > 0) vita2d_draw_rectangle(cx - size * 0.31f, ry, w, 1, col);
        else         vita2d_draw_rectangle(cx + size * 0.31f - w, ry, w, 1, col);
    }
}

/* ---------------------------------------------------------- button hints
 *
 * The face buttons are drawn as shapes, not spelled out and not
 * approximated with punctuation. "/\" for triangle was unreadable, and
 * writing "TRIANGLE" makes the hint bar longer than the hints. Shoulder and
 * system buttons become small labelled pills.
 */
enum { GLY_CROSS, GLY_CIRCLE, GLY_TRI, GLY_SQR,
       GLY_L, GLY_R, GLY_START, GLY_SELECT, GLY_DPAD };

static float ui_glyph(float x, float y, int g)
{
    const float r   = 11.0f;
    unsigned int bg = RGBA8(0x2A, 0x25, 0x3E, 0xFF);
    unsigned int fg = RGBA8(0xD8, 0xD3, 0xE8, 0xFF);
    const char *lab = NULL;

    switch (g) {
    case GLY_L:      lab = "L";   break;
    case GLY_R:      lab = "R";   break;
    case GLY_START:  lab = "START";  break;
    case GLY_SELECT: lab = "SELECT"; break;
    case GLY_DPAD:   lab = "D-PAD";  break;
    default: break;
    }

    if (lab) {
        float w = (float)strlen(lab) * 8.0f + 14.0f;
        ui_round_rect(x, y - r, w, r * 2, 7, bg);
        ui_text_b(x + 7, y + 5, fg, 0.70f, lab);
        return w;
    }

    vita2d_draw_fill_circle(x + r, y, r, bg);
    switch (g) {
    case GLY_CROSS:
        vita2d_draw_line(x + r - 5, y - 5, x + r + 5, y + 5, fg);
        vita2d_draw_line(x + r - 5, y + 5, x + r + 5, y - 5, fg);
        vita2d_draw_line(x + r - 5, y - 4, x + r + 5, y + 6, fg);
        vita2d_draw_line(x + r - 5, y + 6, x + r + 5, y - 4, fg);
        break;
    case GLY_CIRCLE:
        vita2d_draw_fill_circle(x + r, y, 6.0f, fg);
        vita2d_draw_fill_circle(x + r, y, 4.0f, bg);
        break;
    case GLY_TRI:
        ui_triangle(x + r, y + 1, 13, 2, fg);
        ui_triangle(x + r, y + 3, 8, 2, bg);
        break;
    case GLY_SQR:
        ui_round_rect(x + r - 6, y - 6, 12, 12, 2, fg);
        ui_round_rect(x + r - 4, y - 4, 8, 8, 1, bg);
        break;
    }
    return r * 2;
}

/* Draw a row of (glyph, label) hints, returning where it ended. */
static float ui_hints(float x, float y, const int *glyphs,
                      const char *const *labels, int n)
{
    for (int i = 0; i < n; i++) {
        x += ui_glyph(x, y, glyphs[i]) + 6.0f;
        ui_text(x, y + 6, RGBA8(0x9A, 0x94, 0xAD, 0xFF), 0.76f, labels[i]);
        x += (float)strlen(labels[i]) * 8.4f + 20.0f;
    }
    return x;
}

typedef struct {
    char id[64];
    char name[128];
    char year[16];
    /* Percent-encoding can triple a URL's length, so this and the escape
     * buffer must be sized together. Addons that proxy posters through a
     * query string produce URLs well past 512 bytes, and a silently
     * truncated URL 404s every time -- which looks like "some thumbnails
     * never load" rather than like a bug. */
    char poster_url[1024];
    vita2d_texture *tex;      /* NULL until lazily fetched */
    /* Posters fail for ordinary reasons -- a slow remote image, an ffmpeg
     * timeout server-side, an occasional 404 -- and a single flag meant one
     * failure left the cell blank for the rest of the session. Count
     * attempts instead and allow a few, spaced out. */
    int  tex_attempts;
    int  tex_next_try;
} item_t;

/* Debrid playback URLs carry a whole encrypted config in the path, so keys
 * routinely run past 1500 characters. The original 256-byte buffer silently
 * truncated them; the resulting URL decoded to a cut-off path and every such
 * source 404'd, which looked exactly like a dead source. */
#define VS_KEY_MAX 2048

typedef struct {
    char key[VS_KEY_MAX];
    char file[112];        /* release name */
    char quality[16];      /* 2160p DV, 1080p, ... */
    char size[16];
    char seeders[8];
    char provider[24];     /* tracker, or the addon */
    char langs[20];        /* EN/ES/IT */
    int  direct;           /* resolved link vs torrent */
} stream_t;

enum { SCR_CATALOG, SCR_STREAMS, SCR_EPISODES, SCR_PLAYING };
enum { IME_FOR_NONE, IME_FOR_ADDR, IME_FOR_SEARCH };

static int  g_ime_purpose = IME_FOR_NONE;
static char g_search_term[VS_IME_MAX];   /* empty = browsing, not searching */

/* ---------------------------------------------------------- browse state
 *
 * Content type and genre. Genres are Cinemeta's fixed vocabulary; asking
 * for one an addon does not know just returns an empty catalog, which is
 * harmless. Index 0 means "no genre filter". */
static char g_type[12] = "movie";

static const char *const GENRES[] = {
    "All", "Action", "Adventure", "Animation", "Comedy", "Crime",
    "Documentary", "Drama", "Family", "Fantasy", "History", "Horror",
    "Music", "Mystery", "Romance", "Sci-Fi", "Thriller", "War", "Western",
};
#define NGENRES ((int)(sizeof(GENRES) / sizeof(GENRES[0])))

static int g_genre_sel;          /* index into GENRES; 0 = all */
static int g_browse_open;        /* browse overlay visible */
static int g_browse_type;        /* highlighted tab: 0 movies, 1 series */
static int g_browse_row;         /* highlighted genre row */
static int g_browse_scroll;

/* ------------------------------------------------------------ episodes */
#define MAX_EPISODES 400

typedef struct {
    char id[64];                 /* videoId, becomes the /streams id */
    int  season, episode;
    char title[96];
} ep_t;

static ep_t g_eps[MAX_EPISODES];
static int  g_ep_count, g_ep_sel, g_ep_scroll;
static char g_meta_name[128];    /* series title, for the episode header */
static int  g_meta_row_n;        /* 0 while the header row is pending */

/* id the current stream list was fetched for -- the movie id from the
 * catalog or the episode videoId -- so subtitles hit the right title. */
static char g_cur_id[64];

static item_t    g_items[MAX_ITEMS];
static int       g_item_count;
static int       g_sel;
static int       g_scroll;        /* index of first visible row * COLS */

static stream_t  g_streams[MAX_STREAMS];
static int       g_stream_count;
static int       g_stream_sel;
static int       g_stream_scroll;

/* ---------------------------------------------------------- subtitles
 *
 * Cues come from the addon protocol as a flat timed list, already parsed
 * server-side, so nothing here has to understand SRT or WebVTT. Times are
 * absolute in the source, which means they survive seeking without being
 * refetched.
 */
#define MAX_CUES     3000
#define MAX_SUBTRACK 24

typedef struct {
    long start_ms, end_ms;
    char text[152];
} sub_cue;

typedef struct {
    char key[512];      /* base64 of the subtitle URL */
    char lang[8];
    char label[42];
} sub_track;

static sub_cue   *g_cues;            /* allocated on first use */
static int        g_cue_count;
static int        g_cue_hint;        /* last matched index, for a fast scan */
/* Audio tracks reuse the subtitle track shape: an id, a language code and
 * a description. */
static sub_track  g_audtracks[MAX_SUBTRACK];
static int        g_audtrack_count;
static int        g_aud_picker;

static sub_track  g_subtracks[MAX_SUBTRACK];
static int        g_subtrack_count;
static int        g_subtrack_sel;    /* -1 = off */
static int        g_sub_picker;      /* picker overlay visible */
static int        g_pick_scroll;     /* first visible row in either picker */
static int        g_sub_loading;


static int       g_screen = SCR_CATALOG;
static int       g_seek_base;
static char      g_status[128] = "";

/* ------------------------------------------------------------- parsing */

static void on_catalog_row(char **f, int n)
{
    item_t *it;
    if (n < 4 || g_item_count >= MAX_ITEMS) return;

    it = &g_items[g_item_count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->id,   sizeof(it->id),   "%s", f[0]);
    snprintf(it->name, sizeof(it->name), "%s", f[1]);
    snprintf(it->year, sizeof(it->year), "%s", f[2]);
    snprintf(it->poster_url, sizeof(it->poster_url), "%s", f[3]);
}

/* /meta rows: the first is the header (name, year, runtime, description),
 * the rest are one episode each (videoId, season, episode, title). */
static void on_meta_row(char **f, int n)
{
    ep_t *e;
    if (n < 4) return;
    if (g_meta_row_n++ == 0) {
        snprintf(g_meta_name, sizeof(g_meta_name), "%s", f[0]);
        return;
    }
    if (g_ep_count >= MAX_EPISODES) return;
    e = &g_eps[g_ep_count];
    memset(e, 0, sizeof(*e));
    snprintf(e->id, sizeof(e->id), "%s", f[0]);
    e->season  = (int)strtol(f[1], NULL, 10);
    e->episode = (int)strtol(f[2], NULL, 10);
    snprintf(e->title, sizeof(e->title), "%s", f[3]);
    if (e->id[0]) g_ep_count++;
}

static void on_audtrack_row(char **f, int n)
{
    sub_track *t;
    if (n < 3 || g_audtrack_count >= MAX_SUBTRACK) return;
    t = &g_audtracks[g_audtrack_count++];
    snprintf(t->key,   sizeof(t->key),   "%s", f[0]);
    snprintf(t->lang,  sizeof(t->lang),  "%s", f[1]);
    snprintf(t->label, sizeof(t->label), "%s", f[2]);
}

static void on_subtrack_row(char **f, int n)
{
    sub_track *t;
    if (n < 3 || g_subtrack_count >= MAX_SUBTRACK) return;
    t = &g_subtracks[g_subtrack_count++];
    snprintf(t->key,   sizeof(t->key),   "%s", f[0]);
    snprintf(t->lang,  sizeof(t->lang),  "%s", f[1]);
    snprintf(t->label, sizeof(t->label), "%s", f[2]);
}

static void on_cue_row(char **f, int n)
{
    sub_cue *c;
    if (n < 3 || !g_cues || g_cue_count >= MAX_CUES) return;
    c = &g_cues[g_cue_count++];
    c->start_ms = strtol(f[0], NULL, 10);
    c->end_ms   = strtol(f[1], NULL, 10);
    snprintf(c->text, sizeof(c->text), "%s", f[2]);
}

/* Cue covering the given time, or NULL. Playback advances monotonically, so
 * resuming the scan from the last hit makes this O(1) in the common case
 * instead of a linear search over a few thousand cues every frame. */
static const sub_cue *cue_at(long ms)
{
    int i;
    if (!g_cues || g_cue_count == 0) return NULL;

    if (g_cue_hint >= g_cue_count) g_cue_hint = 0;
    if (g_cue_hint > 0 && g_cues[g_cue_hint].start_ms > ms) g_cue_hint = 0;

    for (i = g_cue_hint; i < g_cue_count; i++) {
        if (g_cues[i].end_ms < ms) { g_cue_hint = i + 1; continue; }
        if (g_cues[i].start_ms > ms) return NULL;   /* gap between cues */
        g_cue_hint = i;
        return &g_cues[i];
    }
    return NULL;
}

static void on_stream_row(char **f, int n)
{
    stream_t *s;
    if (n < 2 || g_stream_count >= MAX_STREAMS) return;

    /* Refuse rather than truncate. A clipped key produces a URL that 404s
     * server-side with nothing on the Vita to explain why, so drop the row
     * and say so in the log instead. */
    if (strlen(f[0]) >= VS_KEY_MAX) {
        vs_log("stream key too long (%d bytes, max %d) -- skipped: %s",
               (int)strlen(f[0]), VS_KEY_MAX - 1, f[1]);
        return;
    }

    s = &g_streams[g_stream_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->key,  sizeof(s->key),  "%s", f[0]);
    snprintf(s->file, sizeof(s->file), "%s", f[1]);

    /* Older middleware sends 3 fields, newer sends 8. Fill what arrived so
     * a mismatched pair still lists sources instead of showing nothing. */
    if (n >= 8) {
        snprintf(s->quality,  sizeof(s->quality),  "%s", f[2]);
        snprintf(s->size,     sizeof(s->size),     "%s", f[3]);
        snprintf(s->seeders,  sizeof(s->seeders),  "%s", f[4]);
        snprintf(s->provider, sizeof(s->provider), "%s", f[5]);
        snprintf(s->langs,    sizeof(s->langs),    "%s", f[6]);
        s->direct = (strcmp(f[7], "direct") == 0);
    } else if (n >= 3) {
        s->direct = (strcmp(f[2], "direct") == 0);
    }
}

/* ------------------------------------------------------------- worker
 *
 * Every network call used to run on the render thread, freezing the UI for
 * as long as the server took. A single background worker now owns all
 * socket work; the main thread only ever touches the result.
 *
 * GPU work deliberately stays on the main thread: the worker returns raw
 * JPEG bytes and main.c turns them into a texture. vita2d allocates GPU
 * memory that is not safe to touch from two threads.
 *
 * Handoff is a single-producer/single-consumer slot. Barriers around the
 * state writes keep the payload pointer visible before the state flip. */

enum { JOB_IDLE = 0, JOB_REQUESTED = 1, JOB_DONE = 2 };
enum { WORK_NONE, WORK_CATALOG, WORK_STREAMS, WORK_POSTER, WORK_SYNC,
       WORK_SUBTRACKS, WORK_SUBS, WORK_AUDTRACKS, WORK_META };


static volatile int  g_job_state = JOB_IDLE;
static volatile int  g_job_kind  = WORK_NONE;
static char          g_job_path[900];
static char         *g_job_result;
static int           g_job_len;
static int           g_job_target;      /* item index, for poster jobs */
static SceUID        g_worker;
static int           g_spin;
static SceUInt64     g_job_started;

/* Belt and braces alongside the socket timeouts: if a job somehow outlives
 * every timeout, the UI recovers instead of wedging. The worker still owns
 * g_job_result, so this only reports -- it never frees behind the worker. */
#define JOB_WATCHDOG_US (45 * 1000000)

/* Flaky wifi drops the odd connection; a bare failure would surface as an
 * empty catalog. Three tries with backoff makes that rare. */
static char *fetch_retry(const char *path, int *out_len)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        char *b = vs_get_all(g_mw_ip, g_mw_port, path, out_len);
        if (b) return b;
        sceKernelDelayThread((attempt + 1) * 700000);
    }
    return NULL;
}

static int worker_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (g_job_state != JOB_REQUESTED) {
            sceKernelDelayThread(4000);
            continue;
        }
        __sync_synchronize();
        g_job_result = fetch_retry(g_job_path, &g_job_len);
        __sync_synchronize();
        g_job_state = JOB_DONE;
    }
    return 0;
}

static int job_submit(int kind, const char *path, int target)
{
    if (g_job_state != JOB_IDLE) return 0;      /* one at a time */
    snprintf(g_job_path, sizeof(g_job_path), "%s", path);
    g_job_kind   = kind;
    g_job_target = target;
    g_job_result  = NULL;
    g_job_len     = 0;
    g_job_started = sceKernelGetProcessTimeWide();
    __sync_synchronize();
    g_job_state = JOB_REQUESTED;
    return 1;
}

/* ------------------------------------------------------------- fetching */

static void free_posters(void)
{
    int freed = 0;

    /* MUST wait for the GPU before freeing.
     *
     * vita2d_free_texture releases the memblock immediately, but draw
     * commands from the previous frame may still be reading it. Freeing
     * underneath the GPU faults it, and a GXM fault on this hardware is an
     * unrecoverable hang -- power button territory, not a crash back to the
     * LiveArea. That was the reload freeze.
     *
     * This is called from job_collect on the CATALOG path, which both the
     * reload button and search go through, so the same fault was reachable
     * two ways. */
    vita2d_wait_rendering_done();

    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].tex) {
            vita2d_free_texture(g_items[i].tex);
            g_items[i].tex = NULL;   /* null after free, not just on reuse */
            freed++;
        }
        g_items[i].tex_attempts = 0;
        g_items[i].tex_next_try = 0;
    }
    if (freed) vs_log("released %d poster textures", freed);
}

/* Percent-encode into a query string. Shared by posters and search. */
static void urlesc(const char *in, char *out, int out_max)
{
    int o = 0;
    for (; *in && o < out_max - 4; in++) {
        if ((*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z') ||
            (*in >= '0' && *in <= '9') || strchr("-_.~", *in)) {
            out[o++] = *in;
        } else {
            o += snprintf(out + o, 4, "%%%02X", (unsigned char)*in);
        }
    }
    out[o] = 0;
}

static void request_catalog(void)
{
    char path[160], esc[64];
    g_search_term[0] = 0;              /* leaving search mode */
    if (g_genre_sel > 0) {
        urlesc(GENRES[g_genre_sel], esc, sizeof(esc));
        snprintf(path, sizeof(path), "/catalog?type=%s&genre=%s",
                 g_type, esc);
    } else {
        snprintf(path, sizeof(path), "/catalog?type=%s", g_type);
    }
    if (job_submit(WORK_CATALOG, path, 0))
        snprintf(g_status, sizeof(g_status), "loading catalog");
}

static void request_audtracks(void)
{
    char path[700];
    snprintf(path, sizeof(path), "/audiotracks?s=%s",
             g_streams[g_stream_sel].key);
    if (job_submit(WORK_AUDTRACKS, path, 0))
        snprintf(g_status, sizeof(g_status), "reading audio tracks");
}

static void request_subtracks(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/subtracks?type=%s&id=%s",
             g_type, g_cur_id);
    if (job_submit(WORK_SUBTRACKS, path, 0)) {
        g_sub_loading = 1;
        snprintf(g_status, sizeof(g_status), "finding subtitles");
    }
}

static void request_subs(int track)
{
    char path[700];
    if (track < 0 || track >= g_subtrack_count) return;
    snprintf(path, sizeof(path), "/subs?s=%s", g_subtracks[track].key);
    if (job_submit(WORK_SUBS, path, track)) {
        g_sub_loading = 1;
        snprintf(g_status, sizeof(g_status), "loading subtitles");
    }
}

/* Ask the server to re-pull its addon list from the linked Stremio account.
 * Means installing an addon in Stremio reaches the Vita without SSH. */
static void request_sync(void)
{
    if (job_submit(WORK_SYNC, "/sync", 0))
        snprintf(g_status, sizeof(g_status), "syncing addons");
}

static void request_search(const char *term)
{
    char path[512], esc[400];

    if (!term[0]) { request_catalog(); return; }

    snprintf(g_search_term, sizeof(g_search_term), "%s", term);
    urlesc(term, esc, sizeof(esc));
    snprintf(path, sizeof(path), "/search?type=%s&q=%s", g_type, esc);

    /* Search results reuse the catalog row format and handler, so the whole
     * result path is identical -- only the endpoint differs. */
    if (job_submit(WORK_CATALOG, path, 0))
        snprintf(g_status, sizeof(g_status), "searching");
    vs_log("search: %s", term);
}

static void request_streams_id(const char *id)
{
    char path[640];
    snprintf(path, sizeof(path), "/streams?type=%s&id=%s", g_type, id);
    if (job_submit(WORK_STREAMS, path, 0)) {
        snprintf(g_cur_id, sizeof(g_cur_id), "%s", id);
        snprintf(g_status, sizeof(g_status), "finding streams");
    }
}

static void request_streams(const item_t *it)
{
    request_streams_id(it->id);
}

/* Series need the episode list before sources can be looked up. */
static void request_meta(const item_t *it)
{
    char path[256];
    snprintf(path, sizeof(path), "/meta?type=%s&id=%s", g_type, it->id);
    if (job_submit(WORK_META, path, 0))
        snprintf(g_status, sizeof(g_status), "loading episodes");
}

/* Movies go straight to sources; series go through the episode list. */
static void open_item(const item_t *it)
{
    if (!strcmp(g_type, "series")) request_meta(it);
    else                           request_streams(it);
}

/* Drop artwork far from the current page. */
static void evict_far_posters(void)
{
    int page  = GRID_COLS * GRID_ROWS;
    int lo    = g_scroll - POSTER_KEEP_PAGES * page;
    int hi    = g_scroll + (POSTER_KEEP_PAGES + 1) * page;
    int freed = 0;

    for (int i = 0; i < g_item_count; i++) {
        if (!g_items[i].tex) continue;
        if (i >= lo && i < hi) continue;

        if (!freed) vita2d_wait_rendering_done();  /* once, not per texture */
        vita2d_free_texture(g_items[i].tex);
        g_items[i].tex = NULL;
        /* Allow a refetch if the user comes back to it. */
        g_items[i].tex_attempts = 0;
        g_items[i].tex_next_try = 0;
        freed++;
    }
    if (freed) vs_log("released %d off-screen posters", freed);
}

/* Queue the next visible poster that hasn't been tried yet. */
static void request_next_poster(void)
{
    int page = GRID_COLS * GRID_ROWS;

    for (int i = g_scroll; i < g_item_count && i < g_scroll + page; i++) {
        item_t *it = &g_items[i];
        char path[3200], esc[3100];   /* 1024 * 3 for escaping, plus slack */

        if (it->tex) continue;
        if (it->tex_attempts >= POSTER_MAX_TRIES) continue;
        if (g_spin < it->tex_next_try) continue;
        if (!it->poster_url[0]) {
            /* No URL at all. The server never sees a request for this, so
             * it cannot appear in the middleware log -- it has to be
             * recorded here or the case is invisible from both ends. */
            it->tex_attempts = POSTER_MAX_TRIES;
            vs_log("no poster URL in metadata: id=%s name=%.60s",
                   it->id, it->name);
            continue;
        }

        urlesc(it->poster_url, esc, sizeof(esc));
        snprintf(path, sizeof(path), "/poster?u=%s", esc);
        if (job_submit(WORK_POSTER, path, i)) {
            it->tex_attempts++;
            /* Space retries out so a run of failures cannot monopolise the
             * single worker and stall the posters that would succeed. */
            it->tex_next_try = g_spin + POSTER_RETRY_FRAMES;
        }
        return;
    }
}

/* Consume a finished job. Runs on the main thread so GPU calls are safe. */
static void job_collect(void)
{
    if (g_job_state == JOB_REQUESTED && g_job_started &&
        sceKernelGetProcessTimeWide() - g_job_started > JOB_WATCHDOG_US) {
        g_job_started = 0;
        snprintf(g_status, sizeof(g_status), "server not responding");
        vs_log("job stuck past watchdog; check the middleware is running");
    }

    if (g_job_state != JOB_DONE) return;
    __sync_synchronize();

    switch (g_job_kind) {
    case WORK_CATALOG:
        free_posters();                 /* waits for the GPU internally */
        g_item_count = g_sel = g_scroll = 0;
        if (!g_job_result) {
            snprintf(g_status, sizeof(g_status), "no server at %s:%d",
                     g_mw_ip, g_mw_port);
        } else {
            for_each_record(g_job_result, on_catalog_row);
            if (g_search_term[0])
                snprintf(g_status, sizeof(g_status), "%d for \"%.90s\"",
                         g_item_count, g_search_term);
            else
                snprintf(g_status, sizeof(g_status), "%d titles",
                         g_item_count);
        }
        break;

    case WORK_STREAMS:
        g_stream_count = g_stream_sel = g_stream_scroll = 0;
        /* Cues are for a specific title, so a new source list invalidates
         * them. Keeping them would caption the wrong film. */
        g_subtrack_count = 0;
        g_subtrack_sel   = -1;
        g_cue_count      = 0;
        g_cue_hint       = 0;
        g_audtrack_count = 0;
        vs_play_set_audio_track(-1);
        if (!g_job_result) {
            snprintf(g_status, sizeof(g_status), "stream lookup failed");
        } else {
            for_each_record(g_job_result, on_stream_row);
            snprintf(g_status, sizeof(g_status), "%d sources",
                     g_stream_count);
            if (g_stream_count > 0) g_screen = SCR_STREAMS;
            else snprintf(g_status, sizeof(g_status), "no sources found");
        }
        break;

    case WORK_META:
        g_ep_count = g_ep_sel = g_ep_scroll = 0;
        g_meta_row_n   = 0;
        g_meta_name[0] = 0;
        if (!g_job_result) {
            snprintf(g_status, sizeof(g_status), "episode lookup failed");
        } else {
            for_each_record(g_job_result, on_meta_row);
            if (g_ep_count > 0) {
                g_screen = SCR_EPISODES;
                snprintf(g_status, sizeof(g_status), "%d episodes",
                         g_ep_count);
            } else {
                snprintf(g_status, sizeof(g_status), "no episodes listed");
            }
        }
        break;

    case WORK_AUDTRACKS:
        g_audtrack_count = 0;
        if (g_job_result) for_each_record(g_job_result, on_audtrack_row);
        snprintf(g_status, sizeof(g_status), "%d audio tracks",
                 g_audtrack_count);
        g_aud_picker  = 1;
        g_pick_scroll = 0;
        break;

    case WORK_SUBTRACKS:
        g_subtrack_count = 0;
        g_sub_loading = 0;
        if (g_job_result) for_each_record(g_job_result, on_subtrack_row);
        snprintf(g_status, sizeof(g_status), "%d subtitle tracks",
                 g_subtrack_count);
        g_sub_picker  = 1;
        g_pick_scroll = 0;
        break;

    case WORK_SUBS:
        g_sub_loading = 0;
        g_cue_count = 0;
        g_cue_hint  = 0;
        if (!g_cues)
            g_cues = malloc(sizeof(sub_cue) * MAX_CUES);
        if (g_cues && g_job_result) {
            for_each_record(g_job_result, on_cue_row);
            g_subtrack_sel = g_job_target;
            snprintf(g_status, sizeof(g_status), "%d cues", g_cue_count);
            vs_log("subtitles: %d cues from track %d", g_cue_count,
                   g_job_target);
        } else {
            g_subtrack_sel = -1;
            snprintf(g_status, sizeof(g_status), "subtitles failed");
        }
        break;

    case WORK_SYNC:
        if (!g_job_result) {
            snprintf(g_status, sizeof(g_status), "sync failed");
        } else if (!strncmp(g_job_result, "ok", 2)) {
            snprintf(g_status, sizeof(g_status), "addons synced");
            vs_log("addon sync: %s", g_job_result);
        } else {
            snprintf(g_status, sizeof(g_status), "no account linked");
        }
        break;

    case WORK_POSTER:
        if (g_job_target < g_item_count) {
            item_t *it = &g_items[g_job_target];

            if (g_job_result && g_job_len > 128)
                it->tex = vita2d_load_JPEG_buffer(g_job_result, g_job_len);

            /* A decode that returns NULL counts as a failure too -- a
             * truncated response is not a valid JPEG. Leaving attempts as
             * they are lets request_next_poster try again later. */
            if (!it->tex && it->tex_attempts >= POSTER_MAX_TRIES)
                vs_log("poster failed (%d bytes) name=%.40s url=%.180s",
                       g_job_len, it->name, it->poster_url);
        }
        break;
    }

    free(g_job_result);
    g_job_result = NULL;
    g_job_kind   = WORK_NONE;
    __sync_synchronize();
    g_job_state  = JOB_IDLE;
}

/* Drive the on-screen keyboard. Must run every frame while it is up. */
static void ime_pump(void)
{
    char text[VS_IME_MAX];
    int  r;

    if (!vs_ime_is_active()) return;

    r = vs_ime_poll(text, sizeof(text));
    if (r == 0) return;                      /* still typing */

    if (r < 0) {                             /* cancelled */
        g_ime_purpose = IME_FOR_NONE;
        return;
    }

    switch (g_ime_purpose) {
    case IME_FOR_ADDR: {
        char ip[VS_IP_MAX];
        int  port = g_mw_port;

        if (!vs_parse_addr(text, ip, sizeof(ip), &port)) {
            /* Keep the old address rather than storing something the socket
             * layer can never reach -- there is no DNS to fall back on. */
            snprintf(g_status, sizeof(g_status), "bad address");
            vs_log("rejected address input: %s", text);
            break;
        }
        snprintf(g_mw_ip, sizeof(g_mw_ip), "%s", ip);
        g_mw_port = port;
        vs_cfg_save(g_mw_ip, g_mw_port);
        vs_log("server set to %s:%d", g_mw_ip, g_mw_port);
        request_catalog();
        break;
    }
    case IME_FOR_SEARCH:
        request_search(text);
        break;
    }
    g_ime_purpose = IME_FOR_NONE;
}

static void open_addr_dialog(void)
{
    char cur[VS_IP_MAX + 8];
    snprintf(cur, sizeof(cur), "%s:%d", g_mw_ip, g_mw_port);
    if (vs_ime_open("Server address (ip:port)", cur, 0) == 0)
        g_ime_purpose = IME_FOR_ADDR;
}

static void open_search_dialog(void)
{
    if (vs_ime_open("Search", g_search_term, 0) == 0)
        g_ime_purpose = IME_FOR_SEARCH;
}

/* ------------------------------------------------------------- drawing */

/* Three dots that pulse in sequence. Reads as activity without the jitter
 * of a spinning ASCII bar, and costs three circles. */
static void ui_dots(float x, float y, int a)
{
    for (int i = 0; i < 3; i++) {
        int   phase = (g_spin / 8 + i) % 3;
        float r     = phase == 0 ? 4.0f : 2.5f;
        int   alpha = phase == 0 ? a : a / 3;
        vita2d_draw_fill_circle(x + i * 13, y, r, WITH_A(C_ACCENT, alpha));
    }
}

/* --------------------------------------------------------- browse overlay
 *
 * Two tabs (Movies / Series) over a genre list, in the same visual language
 * as the track pickers. Nothing is fetched until a choice is applied, so
 * backing out is free. */
#define BROWSE_VIS 8

static rect browse_tab(int i)
{
    rect r; r.x = 300 + i * 184; r.y = 92; r.w = 176; r.h = 38;
    return r;
}

static rect browse_row(int i)
{
    rect r; r.x = 300; r.y = 146 + i * 40; r.w = 360; r.h = 34;
    return r;
}

static void draw_browse(void)
{
    int vis = NGENRES < BROWSE_VIS ? NGENRES : BROWSE_VIS;
    int h   = 96 + vis * 40;

    ui_round_rect(276, 74, 408, h, 14, RGBA8(0x0F, 0x0C, 0x1A, 0xF2));

    for (int i = 0; i < 2; i++) {
        rect r  = browse_tab(i);
        int  on = (i == g_browse_type);
        ui_round_rect(r.x, r.y, r.w, r.h, 9,
                      on ? C_ACCENT : RGBA8(0x1D, 0x19, 0x2C, 0xFF));
        ui_text_b(r.x + (i ? 58 : 55), r.y + 26,
                  on ? RGBA8(0x14, 0x10, 0x22, 0xFF) : C_TEXT, 0.86f,
                  i ? "Series" : "Movies");
    }

    for (int v = 0; v < vis; v++) {
        int  i   = g_browse_scroll + v;
        rect r   = browse_row(v);
        int  sel = (i == g_browse_row);
        if (i >= NGENRES) break;
        ui_round_rect(r.x, r.y, r.w, r.h, 8,
                      sel ? C_ACCENT : RGBA8(0x1D, 0x19, 0x2C, 0xFF));
        ui_text(r.x + 14, r.y + 23,
                sel ? RGBA8(0x14, 0x10, 0x22, 0xFF) : C_TEXT, 0.80f,
                GENRES[i]);
    }

    if (NGENRES > BROWSE_VIS) {
        char pos[24];
        snprintf(pos, sizeof(pos), "%d/%d", g_browse_row + 1, NGENRES);
        ui_text(600, 108, C_DIM, 0.74f, pos);
    }
}

static void draw_catalog(void)
{
    int first = g_scroll;
    int last  = first + GRID_COLS * GRID_ROWS;
    int pages, page;

    /* Header: a soft band rather than text floating on the background. */
    ui_scrim(0, 62, 0x40, 0, 0x1B1728);

    if (g_search_term[0]) {
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "%.80s", g_search_term);
        ui_text_b(30, 40, RGBA8(0x7E, 0x76, 0x9C, 0xFF), 0.74f, "SEARCH");
        ui_text_b(108, 41, C_TEXT, 1.18f, hdr);
    } else {
        char sub[48];
        ui_text_b(30, 41, C_TEXT, 1.28f, "vitastremio");
        snprintf(sub, sizeof(sub), "%s%s%s",
                 strcmp(g_type, "series") ? "Movies" : "Series",
                 g_genre_sel > 0 ? "  -  " : "",
                 g_genre_sel > 0 ? GENRES[g_genre_sel] : "");
        ui_text_b(228, 40, RGBA8(0x7E, 0x76, 0x9C, 0xFF), 0.78f, sub);
    }

    ui_text(660, 40, RGBA8(0x9A, 0x94, 0xAD, 0xFF), 0.84f, g_status);
    if (g_job_state != JOB_IDLE) ui_dots(916, 34, 0xFF);

    for (int i = first; i < last && i < g_item_count; i++) {
        int col = (i - first) % GRID_COLS;
        int row = (i - first) / GRID_COLS;
        int x   = GRID_X + col * CELL_W;
        int y   = GRID_Y + row * CELL_H;
        item_t *it = &g_items[i];
        int sel = (i == g_sel);

        if (sel) {
            /* Glow, then ring. Reads as selection rather than as a border
             * someone forgot to inset. */
            ui_round_rect(x - 9, y - 9, POSTER_W + 18, POSTER_H + 18, 12,
                          RGBA8(0x7B, 0x5C, 0xFF, 0x40));
            ui_round_rect(x - 4, y - 4, POSTER_W + 8, POSTER_H + 8, 9,
                          C_ACCENT);
        } else {
            /* A hint of shadow so unselected art still sits on the page. */
            ui_round_rect(x + 2, y + 4, POSTER_W, POSTER_H, 7,
                          RGBA8(0x00, 0x00, 0x00, 0x50));
        }

        if (it->tex) {
            vita2d_draw_texture(it->tex, x, y);
            ui_mask_corners(x, y, POSTER_W, POSTER_H, 7,
                            sel ? C_ACCENT : C_BG);
        } else {
            ui_round_rect(x, y, POSTER_W, POSTER_H, 7,
                          RGBA8(0x1F, 0x1B, 0x2E, 0xFF));

            if (it->tex_attempts >= POSTER_MAX_TRIES) {
                /* Give up gracefully: wrap the title into the cell so the
                 * item stays identifiable and playable without artwork. */
                /* Break at the last space that fits, so "Masters of the
                 * Universe" does not come out as "Univer" / "se". Only
                 * hard-break a word that cannot fit on a line by itself. */
                char w[20];
                int  line = 0;
                const char *p = it->name;
                const int   MAXC = 14;

                while (*p && line < 7) {
                    int len = 0, brk = 0;
                    while (p[len] && len < MAXC) {
                        if (p[len] == ' ') brk = len;
                        len++;
                    }
                    if (p[len] && brk > 0) len = brk;   /* step back to space */

                    memcpy(w, p, len);
                    w[len] = 0;
                    ui_text(x + 10, y + 30 + line * 21, C_DIM, 0.80f, w);

                    p += len;
                    while (*p == ' ') p++;
                    line++;
                }
            } else {
                ui_dots(x + POSTER_W / 2 - 13, y + POSTER_H / 2, 0xB0);
            }
        }

        /* Year badge, bottom-right of the art. */
        if (it->year[0]) {
            int bw = 50;
            ui_round_rect(x + POSTER_W - bw - 6, y + POSTER_H - 26, bw, 21, 9,
                          RGBA8(0x0C, 0x0A, 0x14, 0xC8));
            ui_text_b(x + POSTER_W - bw + 3, y + POSTER_H - 10, C_TEXT,
                      0.72f, it->year);
        }

        {   /* Truncate at the last space before the limit so titles break
             * between words rather than mid-syllable. */
            char t[26];
            int  lim = 20;
            snprintf(t, sizeof(t), "%.24s", it->name);
            if ((int)strlen(t) > lim) {
                int cut = lim;
                for (int k = lim; k > lim / 2; k--)
                    if (t[k] == ' ') { cut = k; break; }
                t[cut] = 0;
                strcat(t, "...");
            }
            ui_text(x, y + POSTER_H + 21,
                    sel ? C_TEXT : RGBA8(0xA8, 0xA1, 0xBC, 0xFF), 0.86f, t);
        }
    }

    /* Page dots, so paging has somewhere to go rather than just changing. */
    pages = (g_item_count + GRID_COLS * GRID_ROWS - 1) /
            (GRID_COLS * GRID_ROWS);
    page  = g_scroll / (GRID_COLS * GRID_ROWS);
    if (pages > 1 && pages <= 16) {
        float x0 = 480 - (pages - 1) * 7.0f;
        for (int i = 0; i < pages; i++)
            vita2d_draw_fill_circle(x0 + i * 14, 500, i == page ? 4.0f : 2.5f,
                                    i == page ? C_ACCENT
                                              : RGBA8(0x4A, 0x44, 0x60, 0xFF));
    }

    ui_scrim(516, 28, 0, 0x60, 0x0C0A14);
    {
        static const int  g[] = { GLY_CROSS, GLY_SQR, GLY_TRI,
                                  GLY_SELECT, GLY_START };
        static const char *const l[] = { "play", "search", "browse",
                                         "sync", "server" };
        ui_hints(30, 528, g, l, 5);
    }
    {
        char srv[80];
        snprintf(srv, sizeof(srv), "%s:%d", g_mw_ip, g_mw_port);
        ui_text(778, 534, RGBA8(0x6E, 0x67, 0x86, 0xFF), 0.74f, srv);
    }

    if (g_browse_open) draw_browse();
}

#define SROW_H   78
#define SROW_Y   82
#define SROW_MAX 5

/* Shared by the renderer and the touch handler so hit targets cannot drift
 * away from what is drawn -- the same reason ctl_btn() exists. */
static rect stream_row(int i)
{
    rect r;
    r.x = 20;
    r.y = SROW_Y + i * SROW_H;
    r.w = 920;
    r.h = SROW_H - 8;
    return r;
}

/* One hue, varying only in weight.
 *
 * The previous gold/green/blue scale implied a verdict -- blue and grey read
 * as "bad" when a 720p source is often exactly what you want on a 544-line
 * panel. Keeping a single violet and letting only the emphasis change means
 * the badge tells you the resolution without also telling you how to feel
 * about it. */
static unsigned int quality_fill(const char *q)
{
    /* Still a single hue; the darker steps keep some sense of order without
     * implying that a lower resolution is a worse choice. */
    if (strstr(q, "2160")) return RGBA8(0x6B, 0x4E, 0xE0, 0xFF);
    if (strstr(q, "1080")) return RGBA8(0x55, 0x42, 0xA8, 0xFF);
    if (strstr(q, "720"))  return RGBA8(0x44, 0x38, 0x78, 0xFF);
    return RGBA8(0x35, 0x2F, 0x50, 0xFF);
}

static unsigned int quality_ink(const char *q)
{
    /* One ink for every badge.
     *
     * Dark text on the brightest fill was technically higher contrast, but
     * it made 2160p look like a different kind of thing from 1080p -- thin
     * and black against bold and white. Consistency matters more here than
     * the extra contrast, and white on the violet fill is legible anyway. */
    (void)q;
    return RGBA8(0xF4, 0xF2, 0xFA, 0xFF);
}

static void draw_streams(void)
{
    int shown = 0;

    ui_scrim(0, 74, 0x58, 0, 0x1B1728);
    ui_text_b(28, 32, RGBA8(0x7E, 0x76, 0x9C, 0xFF), 0.74f, "SOURCES");
    {
        char t[64];
        snprintf(t, sizeof(t), "%.46s", g_items[g_sel].name);
        ui_text_b(28, 62, C_TEXT, 1.15f, t);
    }
    ui_text(700, 34, RGBA8(0x9A, 0x94, 0xAD, 0xFF), 0.82f, g_status);
    if (g_job_state != JOB_IDLE) ui_dots(916, 26, 0xFF);

    for (int i = g_stream_scroll;
         i < g_stream_count && shown < SROW_MAX; i++, shown++) {
        stream_t *st = &g_streams[i];
        rect  r   = stream_row(shown);
        int   sel = (i == g_stream_sel);
        char  line[96];
        int   x   = r.x + 16;

        ui_round_rect(r.x, r.y, r.w, r.h, 10,
                      sel ? RGBA8(0x2E, 0x25, 0x54, 0xFF)
                          : RGBA8(0x18, 0x15, 0x24, 0xFF));
        if (sel)
            ui_round_rect(r.x, r.y, 4, r.h, 2, C_ACCENT);

        /* Quality badge */
        if (st->quality[0]) {
            int bw = (int)strlen(st->quality) * 10 + 20;
            if (bw < 74) bw = 74;
            ui_round_rect(x, r.y + 12, bw, 28, 9, quality_fill(st->quality));
            ui_text_b(x + 10, r.y + 32, quality_ink(st->quality), 0.80f,
                      st->quality);
            x += bw + 12;
        }

        /* Spelled out rather than encoded in a dot.
         *
         * A small filled-vs-hollow circle was too subtle to read at this
         * size, and when every source in a list is instant there is nothing
         * to contrast it against -- so it looked broken even when correct.
         * A word is unambiguous, and absence of the chip is just as
         * informative as its presence. */
        if (st->direct) {
            ui_round_rect(x, r.y + 12, 76, 28, 9,
                          RGBA8(0x1E, 0x4D, 0x3A, 0xFF));
            ui_text_b(x + 9, r.y + 32, RGBA8(0x8C, 0xE8, 0xC0, 0xFF),
                      0.72f, "INSTANT");
            x += 88;
        }

        snprintf(line, sizeof(line), "%.58s", st->file);
        ui_text(x, r.y + 30, sel ? C_TEXT : RGBA8(0xC4, 0xBE, 0xD6, 0xFF),
                0.90f, line);

        /* Languages get their own slot on the right so they are always
         * visible -- appended to the detail line they were the first thing
         * to be truncated away, which is why they never showed. */
        if (st->langs[0]) {
            int lw = (int)strlen(st->langs) * 9 + 18;
            ui_round_rect(r.x + r.w - lw - 14, r.y + 12, lw, 28, 9,
                          RGBA8(0x2C, 0x26, 0x44, 0xFF));
            ui_text_b(r.x + r.w - lw - 5, r.y + 32,
                      RGBA8(0xC6, 0xBE, 0xE0, 0xFF), 0.78f, st->langs);
        }

        {   /* Second line: what you actually compare sources on.
             *
             * The provider is skipped when the title already starts with it
             * -- several addons put their own name in both, and repeating it
             * pushed the useful fields off the end of the row. */
             int o = 0, plen = (int)strlen(st->provider);
             line[0] = 0;

             if (plen > 0 && strncmp(st->file, st->provider, plen) != 0)
                 o += snprintf(line + o, sizeof(line) - o, "%s",
                               st->provider);
             if (st->size[0])
                 o += snprintf(line + o, sizeof(line) - o, "%s%s",
                               o ? "   -   " : "", st->size);
             /* Seeder counts are gone: with debrid sources they are
              * usually absent or zero, and they say nothing about whether
              * the stream will play. Language is what actually decides
              * between two otherwise identical releases. */
             /* State the absence explicitly. A blank space reads as a
              * layout bug; "language not listed" makes clear the addon
              * simply did not supply it. */
             o += snprintf(line + o, sizeof(line) - o, "%s%s",
                           o ? "   -   " : "",
                           st->langs[0] ? st->langs : "language not listed");

             ui_text(r.x + 16, r.y + 58, RGBA8(0x94, 0x8D, 0xAE, 0xFF),
                     0.80f, line);
        }
    }

    if (g_stream_count > SROW_MAX) {
        char pos[32];
        snprintf(pos, sizeof(pos), "%d / %d", g_stream_sel + 1,
                 g_stream_count);
        ui_text_b(858, 534, RGBA8(0xB6, 0xAE, 0xD0, 0xFF), 0.78f, pos);
    }

    ui_scrim(514, 30, 0, 0x60, 0x0C0A14);
    {
        static const int  g[] = { GLY_CROSS, GLY_CIRCLE, GLY_L, GLY_R };
        static const char *const l[] = { "play", "back", "", "page" };
        float x = ui_hints(28, 528, g, l, 4);
        /* Legend for the instant marker, using the same dot the rows draw
         * rather than describing it in words. */
        vita2d_draw_fill_circle(x + 6, 528, 5.0f, C_ACCENT);
        ui_text(x + 18, 534, RGBA8(0x9A, 0x94, 0xAD, 0xFF), 0.76f,
                "plays instantly");
    }
}

#define EROW_H   46
#define EROW_MAX 9

static rect ep_row(int i)
{
    rect r; r.x = 20; r.y = 84 + i * EROW_H; r.w = 920; r.h = EROW_H - 6;
    return r;
}

static void draw_episodes(void)
{
    int shown = 0;

    ui_scrim(0, 74, 0x58, 0, 0x1B1728);
    ui_text_b(28, 32, RGBA8(0x7E, 0x76, 0x9C, 0xFF), 0.74f, "EPISODES");
    {
        char t[64];
        snprintf(t, sizeof(t), "%.46s",
                 g_meta_name[0] ? g_meta_name : g_items[g_sel].name);
        ui_text_b(28, 62, C_TEXT, 1.15f, t);
    }
    ui_text(700, 34, RGBA8(0x9A, 0x94, 0xAD, 0xFF), 0.82f, g_status);
    if (g_job_state != JOB_IDLE) ui_dots(916, 26, 0xFF);

    for (int i = g_ep_scroll;
         i < g_ep_count && shown < EROW_MAX; i++, shown++) {
        ep_t *e   = &g_eps[i];
        rect  r   = ep_row(shown);
        int   sel = (i == g_ep_sel);
        char  tag[24], line[104];

        ui_round_rect(r.x, r.y, r.w, r.h, 9,
                      sel ? RGBA8(0x2E, 0x25, 0x54, 0xFF)
                          : RGBA8(0x18, 0x15, 0x24, 0xFF));
        if (sel)
            ui_round_rect(r.x, r.y, 4, r.h, 2, C_ACCENT);

        snprintf(tag, sizeof(tag), "S%d E%d", e->season, e->episode);
        {
            int bw = (int)strlen(tag) * 10 + 20;
            ui_round_rect(r.x + 14, r.y + 7, bw, 26, 8,
                          RGBA8(0x55, 0x42, 0xA8, 0xFF));
            ui_text_b(r.x + 24, r.y + 26, RGBA8(0xF4, 0xF2, 0xFA, 0xFF),
                      0.74f, tag);
            snprintf(line, sizeof(line), "%.62s", e->title);
            ui_text(r.x + bw + 30, r.y + 27,
                    sel ? C_TEXT : RGBA8(0xC4, 0xBE, 0xD6, 0xFF), 0.88f,
                    line);
        }
    }

    if (g_ep_count > EROW_MAX) {
        char pos[32];
        snprintf(pos, sizeof(pos), "%d / %d", g_ep_sel + 1, g_ep_count);
        ui_text_b(858, 534, RGBA8(0xB6, 0xAE, 0xD0, 0xFF), 0.78f, pos);
    }

    ui_scrim(514, 30, 0, 0x60, 0x0C0A14);
    {
        static const int  g[] = { GLY_CROSS, GLY_CIRCLE, GLY_L, GLY_R };
        static const char *const l[] = { "sources", "back", "", "page" };
        ui_hints(28, 528, g, l, 4);
    }
}

static int g_show_stats;

/* ------------------------------------------------------------- touch
 *
 * The front panel reports in a 1920x1088 grid while the screen is 960x544,
 * so coordinates are halved. Only taps are used: press and release inside
 * the same control. Dragging is deliberately ignored -- a stray finger
 * resting on the panel should not scrub the film.
 */
static int g_touch_down, g_touch_x, g_touch_y;
static int g_tap_x, g_tap_y, g_tapped;

static void touch_poll(void)
{
    SceTouchData t;
    int down;

    g_tapped = 0;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 0) return;

    down = t.reportNum > 0;
    if (down) {
        g_touch_x = t.report[0].x / 2;
        g_touch_y = t.report[0].y / 2;
    } else if (g_touch_down) {
        /* Release: report the last position as a tap. */
        g_tap_x  = g_touch_x;
        g_tap_y  = g_touch_y;
        g_tapped = 1;
    }
    g_touch_down = down;
}

static int tap_in(rect r)
{
    return g_tapped &&
           g_tap_x >= r.x && g_tap_x < r.x + r.w &&
           g_tap_y >= r.y && g_tap_y < r.y + r.h;
}

/* ---------------------------------------------------------- controls
 *
 * Auto-hiding transport bar: a gradient scrim rather than a solid panel, a
 * pill-shaped scrubber with a knob that grows under the finger, and icons
 * drawn as shapes instead of ASCII stand-ins. Fades in and out rather than
 * popping, since a hard cut over moving video is what makes an overlay feel
 * bolted on.
 */
#define CTL_TIMEOUT   300          /* ~5s at 60Hz */
#define CTL_FADE      12           /* refreshes to fade in or out */
#define CTL_BAR_Y     398
#define CTL_BTN       58
#define CTL_SEEK_Y    434
#define CTL_BAR_H     146          /* scrim height below CTL_BAR_Y */

static int   g_ctl_ticks;
static float g_ctl_alpha;

/* rewind, play/pause, forward, audio, subtitles, stop */
#define CTL_NBTN  6
#define CTL_PITCH 118

static rect ctl_btn(int i)
{
    /* Positions come from a pitch rather than a hand-written table, so
     * adding a control cannot silently push the row off the panel. */
    float centre = 480.0f - (CTL_NBTN - 1) * CTL_PITCH / 2.0f + i * CTL_PITCH;
    rect  r;

    r.w = CTL_BTN;
    r.h = CTL_BTN;
    if (i == 1) { r.w += 12; r.h += 12; }      /* play is the primary action */
    r.x = (int)(centre - r.w / 2.0f);
    r.y = CTL_BAR_Y + 62 - (i == 1 ? 6 : 0);
    return r;
}

static int g_ctl_focus = 1;     /* D-pad focus; starts on play/pause */
static int g_aud_probation;     /* frames left to prove a track change works */

static rect ctl_seekbar(void)
{
    /* Narrowed to leave room for the times either side. They used to sit
     * BELOW the bar, which put them in the same horizontal band as the
     * transport buttons -- the rewind button overlapped the elapsed time by
     * a few pixels, and simply shifting the buttons right would have moved
     * the collision onto the total time at the other end. */
    rect r; r.x = 146; r.y = CTL_SEEK_Y; r.w = 668; r.h = 6;
    return r;
}

static void fmt_time(char *out, int n, long secs)
{
    if (secs < 0) secs = 0;
    if (secs >= 3600)
        snprintf(out, n, "%ld:%02ld:%02ld", secs/3600, (secs/60)%60, secs%60);
    else
        snprintf(out, n, "%ld:%02ld", secs/60, secs%60);
}

static int ctl_hot(rect b)
{
    return g_touch_down &&
           g_touch_x >= b.x && g_touch_x < b.x + b.w &&
           g_touch_y >= b.y && g_touch_y < b.y + b.h;
}

static void draw_ctl_icon(int i, rect b, int a)
{
    float cx = b.x + b.w / 2.0f, cy = b.y + b.h / 2.0f;
    unsigned int ink = WITH_A(C_TEXT, a);

    if (i == 3) {                              /* audio: a speaker */
        unsigned int on = (vs_play_audio_track() >= 0)
                          ? WITH_A(C_ACCENT, a) : ink;
        ui_round_rect(cx - 11, cy - 4, 7, 8, 1, on);
        ui_triangle(cx - 1, cy, 18, +1, on);
        vita2d_draw_fill_circle(cx + 9, cy, 6.0f, on);
        vita2d_draw_fill_circle(cx + 9, cy, 4.0f, WITH_A(C_PANEL, a));
        vita2d_draw_rectangle(cx + 2, cy - 7, 8, 14, WITH_A(C_PANEL, a));
        return;
    }
    if (i == 4) {                              /* subtitles: a CC plate */
        unsigned int on = (g_subtrack_sel >= 0) ? WITH_A(C_ACCENT, a) : ink;
        ui_round_rect(cx - 13, cy - 9, 26, 18, 4, on);
        ui_text_b(cx - 9, cy + 5, WITH_A(RGBA8(0x14, 0x10, 0x22, 0xFF), a),
                  0.66f, "CC");
        return;
    }
    if (i == 5) {                              /* stop */
        ui_round_rect(cx - 10, cy - 10, 20, 20, 4, ink);
        return;
    }

    switch (i) {
    case 0:                                    /* rewind: two left triangles */
        ui_triangle(cx - 5, cy, 22, -1, ink);
        ui_triangle(cx + 8, cy, 22, -1, ink);
        break;
    case 1:
        if (vs_play_is_paused()) {
            ui_triangle(cx + 2, cy, 28, +1, ink);
        } else {                               /* pause: two rounded bars */
            ui_round_rect(cx - 9, cy - 13, 6, 26, 3, ink);
            ui_round_rect(cx + 3, cy - 13, 6, 26, 3, ink);
        }
        break;
    case 2:                                    /* forward */
        ui_triangle(cx - 8, cy, 22, +1, ink);
        ui_triangle(cx + 5, cy, 22, +1, ink);
        break;
    }
}

static void draw_controls(void)
{
    long pos = g_seek_base + vs_play_position_s();
    long dur = vs_play_duration_s();
    char left[24], right[24];
    rect sb = ctl_seekbar();
    int  a  = (int)(g_ctl_alpha * 255.0f);
    int  i, fill = 0;

    if (a <= 0) return;

    /* Bottom scrim, and a lighter one at the top for the title. */
    ui_scrim(CTL_BAR_Y - 60, 60 + CTL_BAR_H,
             0, (int)(0xD0 * g_ctl_alpha), 0x0C0A14);
    ui_scrim(0, 70, (int)(0xC0 * g_ctl_alpha), 0, 0x0C0A14);

    if (g_stream_sel < g_stream_count && g_sel < g_item_count)
        ui_text_b(28, 42, WITH_A(C_TEXT, a), 1.18f, g_items[g_sel].name);

    /* Scrubber. Rounded ends make a 6px bar read as a control rather than
     * a hairline. */
    ui_round_rect(sb.x, sb.y, sb.w, sb.h, sb.h / 2.0f,
                  WITH_A(RGBA8(0xFF, 0xFF, 0xFF, 0xFF), a / 5));

    if (dur > 0) {
        fill = (int)((long long)sb.w * pos / dur);
        if (fill < 0) fill = 0;
        if (fill > sb.w) fill = sb.w;
        if (fill > sb.h)
            ui_round_rect(sb.x, sb.y, fill, sb.h, sb.h / 2.0f,
                          WITH_A(C_ACCENT, a));

        {   /* Knob, with a soft halo and a grow-on-touch response. */
            rect hit = sb; hit.y -= 18; hit.h += 36;
            float r = ctl_hot(hit) ? 13.0f : 9.0f;
            vita2d_draw_fill_circle(sb.x + fill, sb.y + sb.h / 2.0f, r + 5,
                                    WITH_A(C_ACCENT, a / 4));
            vita2d_draw_fill_circle(sb.x + fill, sb.y + sb.h / 2.0f, r,
                                    WITH_A(C_TEXT, a));
        }
    }

    /* Flanking the bar, vertically centred on it, clear of the buttons. */
    fmt_time(left, sizeof(left), pos);
    ui_text(30, sb.y + 9, WITH_A(C_TEXT, a), 0.86f, left);
    if (dur > 0) {
        fmt_time(right, sizeof(right), dur);
        ui_text(838, sb.y + 9, WITH_A(C_DIM, a), 0.86f, right);
    }

    for (i = 0; i < CTL_NBTN; i++) {
        rect b   = ctl_btn(i);
        int  hot = ctl_hot(b) || (i == g_ctl_focus);
        float r  = b.w / 2.0f;
        float cx = b.x + r, cy = b.y + b.h / 2.0f;

        if (i == 1) {
            /* Play/pause is the primary action: filled accent with a halo,
             * so the eye lands on it before the secondary controls. */
            vita2d_draw_fill_circle(cx, cy, r + (hot ? 7 : 4),
                                    WITH_A(C_ACCENT, a / 5));
            vita2d_draw_fill_circle(cx, cy, r,
                                    WITH_A(C_ACCENT, hot ? a : a * 9 / 10));
        } else {
            vita2d_draw_fill_circle(cx, cy, r,
                                    WITH_A(C_PANEL, hot ? a : a * 3 / 4));
        }

        /* Focus ring, so the D-pad has somewhere visible to be. Drawn for
         * the focused control whether or not a finger is on the screen. */
        if (i == g_ctl_focus) {
            vita2d_draw_fill_circle(cx, cy, r + 5, WITH_A(C_ACCENT, a / 3));
            vita2d_draw_fill_circle(cx, cy, r + 2, WITH_A(C_ACCENT, a));
            vita2d_draw_fill_circle(cx, cy, r,
                                    WITH_A(i == 1 ? C_ACCENT : C_PANEL, a));
        }
        draw_ctl_icon(i, b, a);
    }
}

static rect sub_row(int i)
{
    rect r; r.x = 300; r.y = 118 + i * 40; r.w = 360; r.h = 34;
    return r;
}

#define PICK_VIS 9      /* rows visible at once */

/* Keep the highlighted row on screen. Without this the selection walked off
 * the bottom of the list and became invisible -- the list drew its first
 * nine entries no matter where the cursor actually was. */
static void picker_follow(int selected, int count)
{
    int row = selected + 1;                  /* row 0 is the default/off */
    int n   = count + 1;

    while (row < g_pick_scroll) {
        g_pick_scroll--;
        if (g_pick_scroll < 0) { g_pick_scroll = 0; break; }
    }
    while (row >= g_pick_scroll + PICK_VIS) g_pick_scroll++;
    if (g_pick_scroll > n - PICK_VIS) g_pick_scroll = n - PICK_VIS;
    if (g_pick_scroll < 0) g_pick_scroll = 0;
}

/* Shared list overlay for both track pickers. */
static void draw_picker(const char *title, const sub_track *rows, int count,
                        int selected, int loading, const char *empty)
{
    int n    = count + 1;
    int vis  = n < PICK_VIS ? n : PICK_VIS;
    int h    = 96 + vis * 40;

    ui_round_rect(276, 74, 408, h, 14, RGBA8(0x0F, 0x0C, 0x1A, 0xF2));
    ui_text_b(300, 108, C_TEXT, 1.0f, title);

    if (n > PICK_VIS) {
        char pos[24];
        snprintf(pos, sizeof(pos), "%d/%d", selected + 2, n);
        ui_text(600, 108, C_DIM, 0.74f, pos);
    }

    for (int v = 0; v < vis; v++) {
        int  i   = g_pick_scroll + v;
        rect r   = sub_row(v);
        int  sel = (i == selected + 1);
        char lbl[64];

        if (i >= n) break;

        ui_round_rect(r.x, r.y, r.w, r.h, 8,
                      sel ? C_ACCENT : RGBA8(0x1D, 0x19, 0x2C, 0xFF));
        if (i == 0)
            snprintf(lbl, sizeof(lbl), "%s", empty);
        else
            snprintf(lbl, sizeof(lbl), "%-3s  %.28s",
                     rows[i - 1].lang, rows[i - 1].label);
        ui_text(r.x + 14, r.y + 23,
                sel ? RGBA8(0x14, 0x10, 0x22, 0xFF) : C_TEXT, 0.80f, lbl);
    }

    if (count == 0)
        ui_text(300, 176, C_DIM, 0.78f,
                loading ? "searching..." : "none available");
}

static void draw_sub_picker(void)
{
    picker_follow(g_subtrack_sel, g_subtrack_count);
    draw_picker("Subtitles", g_subtracks, g_subtrack_count,
                g_subtrack_sel, g_sub_loading, "Off");
}

static void draw_aud_picker(void)
{
    picker_follow(vs_play_audio_track(), g_audtrack_count);
    draw_picker("Audio track", g_audtracks, g_audtrack_count,
                vs_play_audio_track(), 0, "Default");
}

/* Wrap a cue onto at most two lines and draw it above the transport bar. */
static void draw_subtitle(void)
{
    long ms = (g_seek_base + vs_play_position_s()) * 1000;
    const sub_cue *c = cue_at(ms);
    char l1[96], l2[96];
    int  n, cut;

    if (!c) return;

    n = (int)strlen(c->text);
    l2[0] = 0;
    if (n <= 46) {
        snprintf(l1, sizeof(l1), "%s", c->text);
    } else {
        cut = 46;
        for (int i = 46; i > 20; i--)
            if (c->text[i] == ' ') { cut = i; break; }
        snprintf(l1, sizeof(l1), "%.*s", cut, c->text);
        snprintf(l2, sizeof(l2), "%.46s", c->text + cut + 1);
    }

    /* Drawn as dark text offset behind light text: a plate behind the words
     * would cover more picture than the words do, and pure white on a bright
     * scene is unreadable without some separation. */
    {
        int y = l2[0] ? 462 : 486;
        int w = (int)strlen(l1) * 9;
        ui_text(480 - w / 2 + 2, y + 2, RGBA8(0, 0, 0, 0xC0), 0.98f, l1);
        ui_text(480 - w / 2,     y,     RGBA8(0xFF, 0xFF, 0xFF, 0xFF),
                0.98f, l1);
        if (l2[0]) {
            int w2 = (int)strlen(l2) * 9;
            ui_text(480 - w2 / 2 + 2, y + 28, RGBA8(0, 0, 0, 0xC0), 0.98f, l2);
            ui_text(480 - w2 / 2,     y + 26, RGBA8(0xFF, 0xFF, 0xFF, 0xFF),
                    0.98f, l2);
        }
    }
}

/* Primitive counters, drawable on any screen. The catalog issues roughly
 * three times what playback does, so it has to be visible there -- that is
 * where a pool overrun would happen. */
static void draw_prim_stats(void)
{
    char l[160];
    long b = prim_bytes();

    snprintf(l, sizeof(l),
             "rect %ld  circ %ld  glyph %ld   pool ~%ldKB  peak ~%ldKB / %ldKB",
             g_prim_rect, g_prim_circ, g_prim_text,
             b / 1024, g_prim_peak / 1024, (long)VITA2D_POOL_BYTES / 1024);

    ui_round_rect(12, 8, 936, 30, 9, RGBA8(0x0C, 0x0A, 0x14, 0xE6));
    ui_text(26, 29, b > VITA2D_POOL_BYTES / 2 ? C_ACCENT : C_TEXT, 0.80f, l);
}

static void draw_playing(void)
{
    vs_play_draw();
    if (g_subtrack_sel >= 0) draw_subtitle();

    {
        /* Ease toward visible or hidden rather than snapping. A hard cut
         * over moving video is most of what makes an overlay feel bolted
         * on; a fifth of a second of fade is enough to fix that. */
        float target = (vs_play_is_paused() || g_ctl_ticks > 0) ? 1.0f : 0.0f;
        float d      = 1.0f / CTL_FADE;

        if (g_ctl_alpha < target) {
            g_ctl_alpha += d;
            if (g_ctl_alpha > target) g_ctl_alpha = target;
        } else if (g_ctl_alpha > target) {
            g_ctl_alpha -= d;
            if (g_ctl_alpha < target) g_ctl_alpha = target;
        }
    }
    if (g_ctl_alpha > 0.0f) draw_controls();
    if (g_sub_picker) draw_sub_picker();
    if (g_aud_picker) draw_aud_picker();

    if (g_show_stats) {
        vs_play_stat st;
        char l1[128], l2[128], l3[160];

        vs_play_stats(&st);

        /* The hold histogram is the real diagnostic. Healthy 23.976 playback
         * puts everything in the 2 and 3 columns; any count in 1/4/5 is a
         * visible hitch. An average alone cannot show this. */
        snprintf(l3, sizeof(l3),
                 "prims  rect %ld  circ %ld  glyph %ld   pool ~%ldKB peak "
                 "~%ldKB of %ldKB",
                 g_prim_rect, g_prim_circ, g_prim_text,
                 prim_bytes() / 1024, g_prim_peak / 1024,
                 (long)VITA2D_POOL_BYTES / 1024);
        snprintf(l1, sizeof(l1),
                 "holds  1:%ld  2:%ld  3:%ld  4:%ld  5+:%ld   IRREGULAR %ld",
                 st.hold_hist[1], st.hold_hist[2], st.hold_hist[3],
                 st.hold_hist[4], st.hold_hist[5], st.irregular);
        /* av is the lip-sync error: positive means picture is ahead of
         * sound. Should settle within a few tens of ms and stay there. */
        snprintf(l2, sizeof(l2),
                 "shown %ld  drop %ld  under %ld  resync %ld  q%d  "
                 "sync %+dms  trim %+dms (tot %+dms)  %ld.%02ldHz",
                 st.presented, st.dropped, st.underruns, st.resyncs,
                 st.queued, st.av_ms, st.trim_ms, st.total_ms,
                 st.hz_milli / 1000, (st.hz_milli % 1000) / 10);

        ui_round_rect(12, 10, 936, 74, 10, RGBA8(0x0C, 0x0A, 0x14, 0xE0));
        ui_text(26, 30, st.irregular ? C_ACCENT : C_TEXT, 0.82f, l1);
        ui_text(26, 51, C_DIM, 0.82f, l2);
        ui_text(26, 72, prim_bytes() > VITA2D_POOL_BYTES / 2 ? C_ACCENT
                                                             : C_DIM,
                0.82f, l3);
    }
}

/* ------------------------------------------------------------- input */

static void handle_browse(unsigned int pressed)
{
    if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
        g_browse_type = !g_browse_type;
    if (pressed & SCE_CTRL_DOWN) g_browse_row++;
    if (pressed & SCE_CTRL_UP)   g_browse_row--;
    if (g_browse_row < 0) g_browse_row = 0;
    if (g_browse_row >= NGENRES) g_browse_row = NGENRES - 1;

    while (g_browse_row < g_browse_scroll) g_browse_scroll--;
    while (g_browse_row >= g_browse_scroll + BROWSE_VIS) g_browse_scroll++;

    if (g_tapped) {
        for (int i = 0; i < 2; i++)
            if (tap_in(browse_tab(i))) g_browse_type = i;
        for (int v = 0; v < BROWSE_VIS && g_browse_scroll + v < NGENRES;
             v++) {
            if (!tap_in(browse_row(v))) continue;
            g_browse_row = g_browse_scroll + v;
            pressed |= SCE_CTRL_CROSS;
            break;
        }
    }

    if (pressed & SCE_CTRL_CROSS) {
        snprintf(g_type, sizeof(g_type), "%s",
                 g_browse_type ? "series" : "movie");
        g_genre_sel   = g_browse_row;
        g_browse_open = 0;
        request_catalog();
        return;
    }
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE))
        g_browse_open = 0;
}

static void handle_catalog(unsigned int pressed)
{
    int page = GRID_COLS * GRID_ROWS;

    /* The overlay owns input while it is up. */
    if (g_browse_open) { handle_browse(pressed); return; }

    if (pressed & SCE_CTRL_RIGHT) g_sel++;
    if (pressed & SCE_CTRL_LEFT)  g_sel--;
    if (pressed & SCE_CTRL_DOWN)  g_sel += GRID_COLS;
    if (pressed & SCE_CTRL_UP)    g_sel -= GRID_COLS;
    if (pressed & SCE_CTRL_RTRIGGER) g_sel += page;
    if (pressed & SCE_CTRL_LTRIGGER) g_sel -= page;

    if (g_sel < 0) g_sel = 0;
    if (g_sel >= g_item_count) g_sel = g_item_count - 1;
    if (g_sel < 0) g_sel = 0;

    /* Keep the selection on screen. g_scroll is always a non-negative
     * multiple of GRID_COLS, so these loops terminate at 0 -- but clamp
     * inside rather than after, so the invariant is enforced locally
     * instead of assumed. */
    while (g_sel < g_scroll) {
        g_scroll -= GRID_COLS;
        if (g_scroll < 0) { g_scroll = 0; break; }
    }
    while (g_sel >= g_scroll + page) g_scroll += GRID_COLS;

    /* Touch: first tap selects, a second tap on the same cell opens it.
     * Requiring the second tap means a mis-aimed finger costs nothing. */
    if (g_tapped) {
        int page = GRID_COLS * GRID_ROWS;
        for (int i = g_scroll; i < g_item_count && i < g_scroll + page; i++) {
            rect cell;
            cell.x = GRID_X + ((i - g_scroll) % GRID_COLS) * CELL_W - 6;
            cell.y = GRID_Y + ((i - g_scroll) / GRID_COLS) * CELL_H - 6;
            cell.w = POSTER_W + 12;
            cell.h = POSTER_H + 34;
            if (!tap_in(cell)) continue;
            if (i == g_sel) open_item(&g_items[i]);
            else            g_sel = i;
            return;
        }
    }

    if ((pressed & SCE_CTRL_CROSS) && g_item_count > 0)
        open_item(&g_items[g_sel]);         /* screen flips on completion */

    /* Same combo as during playback, so the counters can be checked on the
     * screen that actually draws the most. */
    if ((pad_now & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_TRIANGLE)) {
        g_show_stats = !g_show_stats;
        g_prim_peak  = 0;                /* fresh peak per inspection */
        return;
    }

    if (pressed & SCE_CTRL_SQUARE)   open_search_dialog();
    if (pressed & SCE_CTRL_START)    open_addr_dialog();
    if (pressed & SCE_CTRL_SELECT)   request_sync();
    if (pressed & SCE_CTRL_TRIANGLE) {      /* browse: type + genre */
        g_browse_open   = 1;
        g_browse_type   = !strcmp(g_type, "series");
        g_browse_row    = g_genre_sel;
        g_browse_scroll = 0;
    }
}

static void handle_streams(unsigned int pressed)
{
    if (pressed & SCE_CTRL_DOWN)     g_stream_sel++;
    if (pressed & SCE_CTRL_UP)       g_stream_sel--;
    if (pressed & SCE_CTRL_RTRIGGER) g_stream_sel += SROW_MAX;
    if (pressed & SCE_CTRL_LTRIGGER) g_stream_sel -= SROW_MAX;

    if (g_stream_sel < 0) g_stream_sel = 0;
    if (g_stream_sel >= g_stream_count) g_stream_sel = g_stream_count - 1;
    if (g_stream_sel < 0) g_stream_sel = 0;

    /* Only 7 rows fit once each carries quality, size, seeders and
     * languages, so the list has to scroll or most sources are unreachable.
     * Clamp inside the loops rather than after, so the invariant is
     * enforced locally. */
    while (g_stream_sel < g_stream_scroll) {
        g_stream_scroll--;
        if (g_stream_scroll < 0) { g_stream_scroll = 0; break; }
    }
    while (g_stream_sel >= g_stream_scroll + SROW_MAX)
        g_stream_scroll++;

    if (pressed & SCE_CTRL_CIRCLE)
        g_screen = (!strcmp(g_type, "series") && g_ep_count > 0)
                       ? SCR_EPISODES : SCR_CATALOG;

    if (g_tapped) {
        for (int row = 0; row < SROW_MAX; row++) {
            int i = g_stream_scroll + row;
            if (i >= g_stream_count) break;
            if (!tap_in(stream_row(row))) continue;
            if (i == g_stream_sel) pressed |= SCE_CTRL_CROSS;
            else                   g_stream_sel = i;
            break;
        }
    }

    if ((pressed & SCE_CTRL_CROSS) && g_stream_count > 0) {
        g_seek_base = 0;
        if (vs_play_start(g_mw_ip, g_mw_port,
                          g_streams[g_stream_sel].key, 0) == 0) {
            g_screen = SCR_PLAYING;
        } else {
            snprintf(g_status, sizeof(g_status), "playback failed to start");
        }
    }
}

static void handle_episodes(unsigned int pressed)
{
    if (pressed & SCE_CTRL_DOWN)     g_ep_sel++;
    if (pressed & SCE_CTRL_UP)       g_ep_sel--;
    if (pressed & SCE_CTRL_RTRIGGER) g_ep_sel += EROW_MAX;
    if (pressed & SCE_CTRL_LTRIGGER) g_ep_sel -= EROW_MAX;

    if (g_ep_sel < 0) g_ep_sel = 0;
    if (g_ep_sel >= g_ep_count) g_ep_sel = g_ep_count - 1;
    if (g_ep_sel < 0) g_ep_sel = 0;

    while (g_ep_sel < g_ep_scroll) {
        g_ep_scroll--;
        if (g_ep_scroll < 0) { g_ep_scroll = 0; break; }
    }
    while (g_ep_sel >= g_ep_scroll + EROW_MAX) g_ep_scroll++;

    if (pressed & SCE_CTRL_CIRCLE) g_screen = SCR_CATALOG;

    if (g_tapped) {
        for (int row = 0; row < EROW_MAX; row++) {
            int i = g_ep_scroll + row;
            if (i >= g_ep_count) break;
            if (!tap_in(ep_row(row))) continue;
            if (i == g_ep_sel) pressed |= SCE_CTRL_CROSS;
            else               g_ep_sel = i;
            break;
        }
    }

    if ((pressed & SCE_CTRL_CROSS) && g_ep_count > 0)
        request_streams_id(g_eps[g_ep_sel].id);  /* flips on completion */
}

static void seek_by(int delta)
{
    long dur    = vs_play_duration_s();
    int  target = g_seek_base + (int)vs_play_position_s() + delta;

    if (target < 0) target = 0;
    /* Don't seek past the end: restarting the transcode at or beyond the
     * duration produces a stream that ends immediately, which looks like a
     * crash rather than like reaching the end of the film. */
    if (dur > 0 && target > dur - 5) target = (int)(dur - 5);
    if (target < 0) target = 0;

    /* Seeking restarts the transcode at a new offset -- crude, but it needs
     * no index and no discontinuity handling in the decoder. */
    vs_play_stop();
    g_seek_base = target;

    if (vs_play_start(g_mw_ip, g_mw_port,
                      g_streams[g_stream_sel].key, target) != 0) {
        /* Must not leave the app on the playback screen after a failed
         * start: nothing is running, and the next frame would call
         * vs_play_stop again on a half-initialised player. */
        vs_log("seek to %ds failed to start playback", target);
        snprintf(g_status, sizeof(g_status), "playback failed");
        vs_play_stop();
        g_screen = SCR_STREAMS;
    }
}

static void toggle_pause(void)
{
    vs_play_set_paused(!vs_play_is_paused());
}

static void stop_playback(void)
{
    vs_play_stop();
    g_sub_picker = 0;
    g_aud_picker = 0;
    g_ctl_focus  = 1;
    g_screen     = SCR_STREAMS;
}

static void ctl_activate(int i)
{
    switch (i) {
    case 0: seek_by(-30);   break;
    case 1: toggle_pause(); break;
    case 2: seek_by(+30);   break;
    case 3:
        if (g_audtrack_count == 0) request_audtracks();
        else                       g_aud_picker  = 1;
        g_pick_scroll = 0;
        break;
    case 4:
        /* Fetch the track list the first time; afterwards just reopen it. */
        if (g_subtrack_count == 0 && !g_sub_loading) request_subtracks();
        else                                         g_sub_picker  = 1;
        g_pick_scroll = 0;
        break;
    case 5: stop_playback(); break;
    }
}

static void handle_playing(unsigned int pressed)
{
    int i;

    /* Audio picker. Choosing a track restarts playback at the current
     * position: the audio stream is the master clock, and swapping it under
     * a running schedule would desync everything downstream. */
    if (g_aud_picker) {
        int n   = g_audtrack_count + 1;
        int cur = vs_play_audio_track();

        if (pressed & SCE_CTRL_DOWN) cur++;
        if (pressed & SCE_CTRL_UP)   cur--;
        if (cur < -1) cur = -1;
        if (cur > g_audtrack_count - 1) cur = g_audtrack_count - 1;

        if (g_tapped) {
            for (i = 0; i < PICK_VIS && g_pick_scroll + i < n; i++) {
                if (!tap_in(sub_row(i))) continue;
                cur = g_pick_scroll + i - 1;
                pressed |= SCE_CTRL_CROSS;
                break;
            }
        }
        vs_play_set_audio_track(cur);

        if (pressed & SCE_CTRL_CROSS) {
            g_aud_picker = 0;
            /* Watch the restart for a few seconds. If the chosen stream
             * cannot be produced, playback would otherwise just die and
             * dump the user back to the source list with no explanation. */
            g_aud_probation = (cur >= 0) ? 300 : 0;
            seek_by(0);
            return;
        }
        if (pressed & SCE_CTRL_CIRCLE) g_aud_picker = 0;
        g_ctl_ticks = CTL_TIMEOUT;
        return;
    }

    /* The picker owns input while it is up. */
    if (g_sub_picker) {
        int n = g_subtrack_count + 1;

        if (pressed & SCE_CTRL_DOWN) g_subtrack_sel++;
        if (pressed & SCE_CTRL_UP)   g_subtrack_sel--;
        if (g_subtrack_sel < -1)     g_subtrack_sel = -1;
        if (g_subtrack_sel > g_subtrack_count - 1)
            g_subtrack_sel = g_subtrack_count - 1;

        if (g_tapped) {
            for (i = 0; i < PICK_VIS && g_pick_scroll + i < n; i++) {
                if (!tap_in(sub_row(i))) continue;
                g_subtrack_sel = g_pick_scroll + i - 1;
                pressed |= SCE_CTRL_CROSS;
                break;
            }
        }

        if (pressed & SCE_CTRL_CROSS) {
            if (g_subtrack_sel < 0) {
                g_cue_count = 0;          /* Off */
                g_sub_picker = 0;
            } else {
                int want = g_subtrack_sel;
                g_subtrack_sel = -1;      /* until the cues actually arrive */
                request_subs(want);
                g_sub_picker = 0;
            }
        }
        if (pressed & SCE_CTRL_CIRCLE) g_sub_picker = 0;
        g_ctl_ticks = CTL_TIMEOUT;
        return;
    }

    /* Any input wakes the transport bar. Touching the screen while it is
     * hidden only reveals it -- the first tap must not also press whatever
     * happens to be under the finger. */
    if (pressed || g_tapped) {
        int was_hidden = (g_ctl_ticks == 0);
        g_ctl_ticks = CTL_TIMEOUT;
        if (was_hidden && g_tapped) return;
    }
    if (g_ctl_ticks > 0) g_ctl_ticks--;

    /* --- touch --- */
    if (g_tapped) {
        for (i = 0; i < CTL_NBTN; i++) {
            if (!tap_in(ctl_btn(i))) continue;
            g_ctl_focus = i;            /* touch moves focus too */
            ctl_activate(i);
            g_ctl_ticks = CTL_TIMEOUT;
            return;
        }

        {   /* Tap the seek bar to jump. Only meaningful with a duration. */
            rect sb = ctl_seekbar();
            rect hit = sb;
            hit.y -= 16; hit.h += 32;         /* generous vertical target */
            if (tap_in(hit) && vs_play_duration_s() > 0) {
                long dur = vs_play_duration_s();
                long want = (long)(g_tap_x - sb.x) * dur / sb.w;
                if (want < 0) want = 0;
                if (want > dur - 5) want = dur - 5;
                seek_by((int)(want - (g_seek_base + vs_play_position_s())));
                g_ctl_ticks = CTL_TIMEOUT;
                return;
            }
        }
    }

    /* --- buttons ---
     *
     * Left/Right move focus along the transport bar and X activates what is
     * focused, so every control is reachable without the touchscreen. The
     * shoulder buttons stay direct seek shortcuts. */
    if (pressed & SCE_CTRL_LEFT) {
        g_ctl_focus--;
        if (g_ctl_focus < 0) g_ctl_focus = CTL_NBTN - 1;
        g_ctl_ticks = CTL_TIMEOUT;
    }
    if (pressed & SCE_CTRL_RIGHT) {
        g_ctl_focus++;
        if (g_ctl_focus >= CTL_NBTN) g_ctl_focus = 0;
        g_ctl_ticks = CTL_TIMEOUT;
    }
    if (pressed & SCE_CTRL_CROSS) { ctl_activate(g_ctl_focus); return; }
    if (pressed & SCE_CTRL_RTRIGGER)  seek_by(+30);
    if (pressed & SCE_CTRL_LTRIGGER)  seek_by(-30);

    /* Diagnostics are behind SELECT+TRIANGLE rather than a bare button.
     * Triangle alone is far too easy to hit by accident mid-film, and once
     * the overlay is up the D-pad silently becomes a sync control. Gating
     * both behind the same combo keeps normal playback free of surprises. */
    if ((pad_now & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_TRIANGLE))
        g_show_stats = !g_show_stats;

    /* Fine A/V tuning, only while the diagnostics overlay is open. */
    if (g_show_stats) {
        static int repeat_hold, repeat_tick;
        unsigned int held = pad_now & (SCE_CTRL_UP | SCE_CTRL_DOWN);
        int step = 0;

        if (pressed & SCE_CTRL_UP)   step = +1000;
        if (pressed & SCE_CTRL_DOWN) step = -1000;

        if (held) {
            int mag;
            repeat_hold++;
            /* Accelerate: 1ms taps, then 5ms, then 20ms after a long hold.
             * Sweeping a few hundred ms one millisecond at a time is not a
             * realistic way to find the value. */
            if (repeat_hold > 180)      mag = 20000;
            else if (repeat_hold > 90)  mag =  5000;
            else                        mag =  1000;

            if (repeat_hold > 24 && (repeat_tick++ % 4) == 0)
                step = (held & SCE_CTRL_UP) ? mag : -mag;
        } else {
            repeat_hold = repeat_tick = 0;
        }

        if (step) vs_play_trim(step);
    }

    if (pressed & SCE_CTRL_CIRCLE) { stop_playback(); return; }

    /* Natural end of stream. Not checked while paused: the threads are idle
     * by design then, which is indistinguishable from finishing. */
    if (!vs_play_is_paused() && !vs_play_running()) {
        if (g_aud_probation > 0) {
            /* Died right after a track change: that stream is unusable, so
             * fall back to the default rather than ending playback. */
            vs_log("audio track %d failed, reverting to default",
                   vs_play_audio_track());
            snprintf(g_status, sizeof(g_status),
                     "that audio track failed - using default");
            vs_play_set_audio_track(-1);
            g_aud_probation = 0;
            seek_by(0);
            return;
        }
        stop_playback();
        return;
    }

    if (g_aud_probation > 0) g_aud_probation--;
}

/* ------------------------------------------------------------- main */

int main(void)
{
    SceCtrlData pad, prev;
    unsigned int pressed;
    int have_cfg;
    int running = 1;

    memset(&prev, 0, sizeof(prev));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                             SCE_TOUCH_SAMPLING_STATE_START);

    vs_log_init();

    {   /* Common dialogs need this before the first sceImeDialogInit. */
        SceCommonDialogConfigParam cdcfg;
        sceCommonDialogConfigParamInit(&cdcfg);
        sceCommonDialogSetConfigParam(&cdcfg);
    }
    have_cfg = vs_cfg_load(g_mw_ip, sizeof(g_mw_ip), &g_mw_port);
    if (have_cfg)
        vs_log("loaded server %s:%d from config", g_mw_ip, g_mw_port);
    else
        vs_log("no config, using default %s:%d", g_mw_ip, g_mw_port);

    /* Explicit, large vertex pool.
     *
     * vita2d builds every primitive for a frame into one pool and does not
     * bounds-check it -- past the end it keeps returning pointers, and the
     * GPU then reads whatever is there as vertex data. That faults the GPU
     * rather than failing in any catchable way, which matches the crash
     * dumps exactly: every thread killed by one external cause, no CPU
     * exception, no leaked memory.
     *
     * This UI issues several hundred primitives per frame, so the default
     * pool is not a comfortable margin. 4MB is cheap next to the 17MB the
     * poster eviction just reclaimed. */
    vita2d_init_advanced(VITA2D_POOL_BYTES);
    /* Explicit: the frame pacing depends on the main loop running at exactly
     * one iteration per 60Hz refresh. */
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(C_BG);
    g_font = vita2d_load_default_pgf();

    if (vs_net_init() < 0) {
        vs_log("vs_net_init failed -- is wifi connected?");
        snprintf(g_status, sizeof(g_status), "network init failed");
    } else {
        g_worker = sceKernelCreateThread("vs_net", worker_main,
                                         0x10000100, 0x20000, 0, 0, NULL);
        if (g_worker < 0) {
            snprintf(g_status, sizeof(g_status), "worker thread failed");
        } else {
            sceKernelStartThread(g_worker, 0, NULL);
            if (have_cfg) {
                request_catalog();
            } else {
                /* Nothing saved yet. Ask up front rather than showing a
                 * connection failure the user has no obvious way to fix. */
                snprintf(g_status, sizeof(g_status), "set server address");
                open_addr_dialog();
            }
        }
    }

    while (running) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~prev.buttons;
        pad_now = pad.buttons;          /* held state, for auto-repeat */
        prev = pad;
        touch_poll();

        /* While the IME is up it owns the buttons. Feeding them to the app
         * as well would fire actions behind the keyboard. */
        ime_pump();
        if (vs_ime_is_active()) { pressed = 0; pad_now = 0; }

        /* Explicit quit. Previously the main loop could not be left at all,
         * so anything that wedged meant holding the power button. */
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) {
            vs_log("quit requested (START+SELECT)");
            if (g_screen == SCR_PLAYING) vs_play_stop();
            running = 0;
            continue;
        }

        switch (g_screen) {
        case SCR_CATALOG:  handle_catalog(pressed);  break;
        case SCR_STREAMS:  handle_streams(pressed);  break;
        case SCR_EPISODES: handle_episodes(pressed); break;
        case SCR_PLAYING:  handle_playing(pressed);  break;
        }

        g_spin++;
        vs_play_gc();          /* release retired frame textures */

        /* The Vita dims and then suspends after a few minutes without input,
         * and watching a film is exactly the case with no input. Poking the
         * power tick resets those timers. DEFAULT resets all of them --
         * DISABLE_AUTO_SUSPEND alone would stop the suspend but still let
         * the screen dim. Once a second is plenty. */
        if (g_screen == SCR_PLAYING && (g_spin % 60) == 0)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        /* Must run every refresh while playing, before drawing. */
        if (g_screen == SCR_PLAYING) vs_play_present();
        job_collect();
        if (g_screen == SCR_CATALOG && g_job_state == JOB_IDLE) {
            /* Evicting on a slow cadence keeps it off the hot path; the
             * memory only matters over minutes of browsing. */
            if ((g_spin % 120) == 0) evict_far_posters();
            request_next_poster();
        }

        g_prim_rect = g_prim_circ = g_prim_line = g_prim_text = 0;

        vita2d_start_drawing();
        vita2d_clear_screen();
        switch (g_screen) {
        case SCR_CATALOG:  draw_catalog();  break;
        case SCR_STREAMS:  draw_streams();  break;
        case SCR_EPISODES: draw_episodes(); break;
        case SCR_PLAYING:  draw_playing();  break;
        }
        if (g_show_stats && g_screen != SCR_PLAYING) draw_prim_stats();
        {   /* Peak is what matters: one heavy frame is enough to overrun
             * the pool, and an average would hide it. */
            long b = prim_bytes();
            if (b > g_prim_peak) {
                g_prim_peak = b;
                if (b > VITA2D_POOL_BYTES / 2)
                    vs_log("frame used ~%ld KB of the %ld KB vertex pool "
                           "(rect %ld circ %ld glyph %ld)",
                           b / 1024, (long)VITA2D_POOL_BYTES / 1024,
                           g_prim_rect, g_prim_circ, g_prim_text);
            }
        }

        vita2d_end_drawing();
        /* Must run every frame the dialog is up or it appears frozen. */
        if (vs_ime_is_active()) vita2d_common_dialog_update();
        vita2d_swap_buffers();
    }

    free_posters();
    vita2d_free_pgf(g_font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
