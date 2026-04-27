#include "protocol.h"
#include <stdio.h>
#include <time.h>

void write_log(const char *level, const char *message) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    fprintf(f, "[%s] [%s] [%s]\n", timestamp, level, message);
    fclose(f);
}