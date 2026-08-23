/* log.h -- append-only file logging to ux0:data/vitastremio.log
 *
 * printf() on a retail Vita goes nowhere unless you have psp2shell or a
 * kernel logger attached. Since the riskiest part of this project is a
 * decoder that may fail in ways only its error codes explain, diagnostics
 * need to survive without a debugger.
 *
 * The file is opened and closed on every line. That is slow -- do not call
 * this per frame in a working build -- but it means the log is complete up
 * to the instant of a hang or crash, which is exactly when it matters. A
 * buffered logger loses the last and most interesting few lines.
 */
#ifndef VS_LOG_H
#define VS_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

#define VS_LOG_PATH "ux0:data/vitastremio.log"

static void vs_log_init(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoRemove(VS_LOG_PATH);          /* fresh log each launch */
}

static void vs_log(const char *fmt, ...)
{
    char line[512];
    int  n;
    SceUID fd;
    SceRtcTick tick;

    sceRtcGetCurrentTick(&tick);
    n = snprintf(line, sizeof(line), "[%8llu] ",
                 (unsigned long long)(tick.tick / 1000ULL) % 100000000ULL);

    {
        va_list ap;
        va_start(ap, fmt);
        n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
        va_end(ap);
    }

    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';

    fd = sceIoOpen(VS_LOG_PATH,
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, line, n);
    sceIoClose(fd);
}

#endif /* VS_LOG_H */
