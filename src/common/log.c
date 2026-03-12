#include "log.h"

// Implementation of JSON-line logging with UTC timestamps.
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

static FILE *g_log_stream = NULL;
static char g_log_path[256] = {0};

void log_set_file(const char *path) {
    if (!path) return;
    if (g_log_stream && strcmp(g_log_path, path) == 0) {
        return;
    }
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    if (g_log_stream) {
        fclose(g_log_stream);
    }
    g_log_stream = fp;
    setvbuf(g_log_stream, NULL, _IOLBF, 0);
    snprintf(g_log_path, sizeof(g_log_path), "%s", path);
}

// Internal formatter.
static void vlogf(const char *level, const char *event, const char *fmt, va_list ap) {
    FILE *out = g_log_stream ? g_log_stream : stdout;
    char ts[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);
    fprintf(out, "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":\"%s\",\"msg\":\"",
            ts, level, event);
    vfprintf(out, fmt, ap);
    fprintf(out, "\"}\n");
    fflush(out);
}

void log_info(const char *event, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vlogf("INFO", event, fmt, ap); va_end(ap);
}

void log_warning(const char *event, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vlogf("WARNING", event, fmt, ap); va_end(ap);
}

void log_error(const char *event, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vlogf("ERROR", event, fmt, ap); va_end(ap);
}


