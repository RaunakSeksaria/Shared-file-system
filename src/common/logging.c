#include "logging.h"

#include <stdio.h>
#include <time.h>

static void log_base(const char *level, const char *event, const char *detail) {
	char ts[64];
	struct tm tm;
	time_t now = time(NULL);
	gmtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
	fprintf(stdout, "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":\"%s\",\"detail\":\"%s\"}\n", ts, level, event, detail ? detail : "");
	fflush(stdout);
}

void log_info(const char *event, const char *detail) { log_base("INFO", event, detail); }
void log_error(const char *event, const char *detail) { log_base("ERROR", event, detail); }


