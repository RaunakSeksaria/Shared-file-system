#ifndef LOG_H
#define LOG_H

// Minimal JSON-line logging helpers. Output goes to stdout.
void log_info(const char *event, const char *fmt, ...);
void log_warning(const char *event, const char *fmt, ...);
void log_error(const char *event, const char *fmt, ...);
void log_set_file(const char *path);

#endif

