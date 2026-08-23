/* http.h -- minimal HTTP/1.1 GET over raw Vita sockets.
 *
 * Deliberately not libcurl: we need a socket we can read incrementally for
 * hours (the A/V streams) and one we can rip out from under a blocked read
 * when the user hits stop. Raw sockets make both trivial.
 *
 * All middleware responses are plain HTTP, no TLS, no redirects, no chunked
 * encoding (the middleware always sets Content-Length or streams until
 * close). That lets this stay under 200 lines.
 */
#ifndef VS_HTTP_H
#define VS_HTTP_H

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VS_NET_HEAP (1 * 1024 * 1024)

/* Every socket gets a finite timeout. Without one, sceNetRecv blocks
 * forever on a stalled connection -- a restarted middleware, a wifi blip, a
 * half-open TCP session -- and the worker thread never returns. The job
 * state stays "in flight", every later action becomes a silent no-op, and
 * the app looks frozen with no way out. That was a real hang, not a
 * theoretical one. */
#define VS_CONNECT_TIMEOUT_US (8 * 1000000)
#define VS_RECV_TIMEOUT_US    (20 * 1000000)

typedef struct {
    int  fd;
    long content_length;   /* -1 = stream until close */
    int  fps_milli;        /* X-Video-FPS, source rate x1000; 0 if absent */
    long duration_s;       /* X-Duration, whole stream seconds; 0 if absent */
    char leftover[8192];   /* body bytes that arrived with the headers */
    int  leftover_len;
    int  leftover_pos;
} vs_conn;

static void *g_net_mem = NULL;

static int vs_net_init(void)
{
    SceNetInitParam p;

    /* SceNet is not resident by default. Without this, sceNetInit fails and
     * every socket call afterwards returns a confusing error. */
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    if (sceNetShowNetstat() == SCE_NET_ERROR_ENOTINIT) {
        g_net_mem = malloc(VS_NET_HEAP);
        p.memory = g_net_mem;
        p.size   = VS_NET_HEAP;
        p.flags  = 0;
        if (sceNetInit(&p) < 0) return -1;
    }
    sceNetCtlInit();
    return 0;
}

/* Blocking connect to host:port. Host must be a dotted-quad IP -- we skip
 * DNS entirely, since the middleware lives at a fixed LAN address. */
static int vs_tcp_connect(const char *ip, int port)
{
    SceNetSockaddrIn addr;
    int fd = sceNetSocket("vs", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    int opt;

    if (fd < 0) return -1;

    /* Set before connect so a black-holed address fails in seconds rather
     * than sitting in TCP retransmit for over a minute. */
    opt = VS_CONNECT_TIMEOUT_US;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO,
                     &opt, sizeof(opt));
    opt = VS_RECV_TIMEOUT_US;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO,
                     &opt, sizeof(opt));

    /* Big receive buffer: the video stream is bursty and the Vita's default
     * is small enough to cause visible stalls on keyframe-heavy scenes. */
    opt = 256 * 1024;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF,
                     &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port   = sceNetHtons(port);
    sceNetInetPton(SCE_NET_AF_INET, ip, &addr.sin_addr);

    if (sceNetConnect(fd, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetSocketClose(fd);
        return -1;
    }
    return fd;
}

/* Issue a GET and consume the response headers. Returns 0 on 200. */
static int vs_get(vs_conn *c, const char *ip, int port, const char *path)
{
    /* Big enough for the longest playback path plus headers. Undersizing
     * this truncates the request line itself, which is the same silent
     * failure the stream key buffer used to have. */
    char req[4096];
    char hdr[8192];
    int  hlen = 0, n, code = 0;
    char *body;

    memset(c, 0, sizeof(*c));
    c->content_length = -1;
    c->fd = vs_tcp_connect(ip, port);
    if (c->fd < 0) return -1;

    if (strlen(path) > sizeof(req) - 256) return -1;   /* would truncate */

    n = snprintf(req, sizeof(req),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "User-Agent: vitastremio/1\r\n"
                 "Connection: close\r\n\r\n",
                 path, ip, port);
    if (sceNetSend(c->fd, req, n, 0) < 0) goto fail;

    /* Read until we see the end of headers. */
    while (hlen < (int)sizeof(hdr) - 1) {
        n = sceNetRecv(c->fd, hdr + hlen, sizeof(hdr) - 1 - hlen, 0);
        if (n <= 0) goto fail;
        hlen += n;
        hdr[hlen] = 0;
        if (strstr(hdr, "\r\n\r\n")) break;
    }

    if (sscanf(hdr, "HTTP/1.%*d %d", &code) != 1 || code != 200) goto fail;

    {
        const char *cl = strstr(hdr, "Content-Length:");
        if (!cl) cl = strstr(hdr, "content-length:");
        if (cl) c->content_length = strtol(cl + 15, NULL, 10);
    }

    {   /* Source frame rate x1000, so 23.976 arrives as 23976. Lets the
         * player time frames exactly instead of assuming a fixed rate. */
        const char *fp = strstr(hdr, "X-Video-FPS:");
        if (!fp) fp = strstr(hdr, "x-video-fps:");
        if (fp) c->fps_milli = (int)strtol(fp + 12, NULL, 10);
    }

    {   /* Stream length, so the progress bar means something. */
        const char *dp = strstr(hdr, "X-Duration:");
        if (!dp) dp = strstr(hdr, "x-duration:");
        if (dp) c->duration_s = strtol(dp + 11, NULL, 10);
    }

    body = strstr(hdr, "\r\n\r\n") + 4;
    c->leftover_len = hlen - (int)(body - hdr);
    if (c->leftover_len > 0)
        memcpy(c->leftover, body, c->leftover_len);
    c->leftover_pos = 0;
    return 0;

fail:
    sceNetSocketClose(c->fd);
    c->fd = -1;
    return -1;
}

/* Read up to len bytes. Returns 0 at end of stream, <0 on error. */
static int vs_read(vs_conn *c, void *buf, int len)
{
    if (c->fd < 0) return -1;

    if (c->leftover_pos < c->leftover_len) {
        int avail = c->leftover_len - c->leftover_pos;
        int take  = avail < len ? avail : len;
        memcpy(buf, c->leftover + c->leftover_pos, take);
        c->leftover_pos += take;
        return take;
    }
    {
        int n = sceNetRecv(c->fd, buf, len, 0);
        /* Distinguish a stall from a finished stream.
         *
         *    >0  data
         *     0  peer closed cleanly -- really the end
         *    <0  timeout or transient error -- the stream may well resume
         *
         * Collapsing the last case into 0 meant a single slow patch on a
         * debrid link looked identical to end-of-file, and playback stopped
         * for good. Callers that can retry now can. */
        return n;
    }
}

static void vs_close(vs_conn *c)
{
    if (c->fd >= 0) {
        sceNetSocketClose(c->fd);
        c->fd = -1;
    }
}

/* Convenience: slurp an entire finite response into a malloc'd buffer. */
static char *vs_get_all(const char *ip, int port, const char *path, int *out_len)
{
    vs_conn c;
    int cap = 65536, len = 0, n;
    char *buf;

    if (vs_get(&c, ip, port, path) < 0) return NULL;

    buf = malloc(cap);
    if (!buf) { vs_close(&c); return NULL; }

    for (;;) {
        if (len + 8192 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        n = vs_read(&c, buf + len, 8192);
        if (n <= 0) break;
        len += n;
    }
    vs_close(&c);
    buf[len] = 0;
    if (out_len) *out_len = len;
    return buf;
}

#endif /* VS_HTTP_H */
