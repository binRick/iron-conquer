#include "debug_log.h"

#include "raylib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *g_log = NULL;

void debug_log_init(void) {
    const char *path = getenv("IRON_CONQUER_DEBUG_LOG");
    if (!path || !*path) return;

    g_log = fopen(path, "w");
    if (!g_log) {
        TraceLog(LOG_WARNING, "debug_log: could not open '%s'", path);
        return;
    }

    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(g_log, "# iron-conquer debug log started %s\n", ts);
    fprintf(g_log, "# format: [T+SECONDS] tag key=value ...\n");
    fflush(g_log);
}

bool debug_log_enabled(void) {
    return g_log != NULL;
}

void debug_log(const char *fmt, ...) {
    if (!g_log) return;

    fprintf(g_log, "[T+%7.3fs] ", GetTime());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

void debug_log_close(void) {
    if (!g_log) return;
    debug_log("shutdown");
    fclose(g_log);
    g_log = NULL;
}
