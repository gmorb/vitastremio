/* config.h -- server address parsing and persistence.
 *
 * The address parser is deliberately Vita-header-free so it can be tested on
 * the host. It is the one piece of this feature that has real edge cases:
 * users will type spaces, omit the port, include "http://", or fat-finger a
 * digit, and a bad parse strands the app with no way to reach the server.
 */
#ifndef VS_CONFIG_H
#define VS_CONFIG_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define VS_CFG_PATH   "ux0:data/vitastremio.cfg"
#define VS_IP_MAX     64

/* Parse "1.2.3.4", "1.2.3.4:8480", " http://1.2.3.4:8480/ " into ip + port.
 * Returns 1 on success, 0 if the host part isn't a valid dotted quad.
 * port is left untouched when the input carries no port. */
static int vs_parse_addr(const char *in, char *ip, int ip_max, int *port)
{
    char tmp[VS_IP_MAX * 2];
    char *p = tmp, *colon, *slash;
    int  n = 0, o = 0;

    if (!in) return 0;

    /* Copy while dropping whitespace -- the IME makes stray spaces easy. */
    for (; *in && o < (int)sizeof(tmp) - 1; in++)
        if (*in != ' ' && *in != '\t') tmp[o++] = *in;
    tmp[o] = 0;

    /* Tolerate a pasted URL. */
    if (!strncmp(p, "http://", 7))  p += 7;
    if (!strncmp(p, "https://", 8)) p += 8;

    slash = strchr(p, '/');
    if (slash) *slash = 0;

    colon = strchr(p, ':');
    if (colon) {
        int v = atoi(colon + 1);
        if (v <= 0 || v > 65535) return 0;
        *port = v;
        *colon = 0;
    }

    /* Validate the dotted quad. Anything else and we'd store an address the
     * socket layer can never connect to, with no DNS to fall back on. */
    {
        const char *q = p;
        int octet = -1;

        for (;;) {
            if (*q >= '0' && *q <= '9') {
                if (octet < 0) octet = 0;
                octet = octet * 10 + (*q - '0');
                if (octet > 255) return 0;
                q++;
            } else if (*q == '.' || *q == 0) {
                if (octet < 0) return 0;      /* empty octet */
                n++;
                octet = -1;
                if (*q == 0) break;
                q++;
            } else {
                return 0;                    /* junk character */
            }
        }
        if (n != 4) return 0;
    }

    if ((int)strlen(p) >= ip_max) return 0;
    strcpy(ip, p);
    return 1;
}

#ifndef VS_CONFIG_HOST_TEST

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

/* Config is two lines of key=value. Rewritten whole on every save. */
static void vs_cfg_save(const char *ip, int port)
{
    char buf[128];
    int  n;
    SceUID fd;

    sceIoMkdir("ux0:data", 0777);
    n = snprintf(buf, sizeof(buf), "ip=%s\nport=%d\n", ip, port);

    fd = sceIoOpen(VS_CFG_PATH,
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

/* Returns 1 if a usable address was loaded, 0 otherwise (first run). */
static int vs_cfg_load(char *ip, int ip_max, int *port)
{
    char buf[256];
    int  n;
    SceUID fd = sceIoOpen(VS_CFG_PATH, SCE_O_RDONLY, 0777);

    if (fd < 0) return 0;
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    {
        char *line, *save = NULL;
        char  got_ip[VS_IP_MAX] = "";
        int   got_port = *port;

        for (line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(line, "ip=", 3))
                snprintf(got_ip, sizeof(got_ip), "%s", line + 3);
            else if (!strncmp(line, "port=", 5))
                got_port = atoi(line + 5);
        }
        if (!got_ip[0]) return 0;
        /* Re-validate on load: a hand-edited file must not brick startup. */
        return vs_parse_addr(got_ip, ip, ip_max, &got_port)
               ? (*port = got_port, 1) : 0;
    }
}

#endif /* VS_CONFIG_HOST_TEST */
#endif /* VS_CONFIG_H */
