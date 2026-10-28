/*
 * tglog.c — see tglog.h.
 *
 * Writes go to /tmp when the process is allowed to (root shell, or a
 * jailbreak app in /Applications); otherwise they fall back to the app's
 * own container tmp directory, which is always writable.
 */
#include "tglog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static FILE *g_log;
static char g_dir[512];

const char *tg_dir(void)
{
    if (g_dir[0]) return g_dir;

    char probe[600];
    snprintf(probe, sizeof(probe), "/tmp/.tg_probe.%d", (int)getpid());
    FILE *t = fopen(probe, "w");
    if (t) {
        fclose(t);
        remove(probe);
        strcpy(g_dir, "/tmp");
        return g_dir;
    }
    const char *e = getenv("TMPDIR");
    if (e && e[0]) snprintf(g_dir, sizeof(g_dir), "%s", e);
    else strcpy(g_dir, ".");
    return g_dir;
}

void tglog_write(const char *fmt, ...)
{
    if (!g_log) {
        char path[600];
        snprintf(path, sizeof(path), "%s/tg.log", tg_dir());
        g_log = fopen(path, "a");
        if (!g_log) return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    fprintf(g_log, "[%ld.%03ld] ", (long)(tv.tv_sec % 10000),
            (long)(tv.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
