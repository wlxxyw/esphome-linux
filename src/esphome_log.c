/**
 * @file esphome_log.c
 * @brief Minimal global leveled logging implementation
 */

#include "esphome_log.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static esplog_level_t g_esphome_log_level = ESPLOG_INFO;

void esphome_log_set_level(esplog_level_t level) {
    if (level >= ESPLOG_DEBUG && level <= ESPLOG_ERROR) {
        g_esphome_log_level = level;
    }
}

esplog_level_t esphome_log_get_level(void) {
    return g_esphome_log_level;
}

void esphome_log_init(void) {
    const char *s = getenv("LOG_LEVEL");
    esplog_level_t level = ESPLOG_INFO;

    if (s == NULL || s[0] == '\0') {
        return; /* keep default Info */
    }

    if (strcasecmp(s, "Debug") == 0) {
        level = ESPLOG_DEBUG;
    } else if (strcasecmp(s, "Info") == 0) {
        level = ESPLOG_INFO;
    } else if (strcasecmp(s, "Warning") == 0 || strcasecmp(s, "Warn") == 0) {
        level = ESPLOG_WARNING;
    } else if (strcasecmp(s, "Error") == 0 || strcasecmp(s, "Err") == 0) {
        level = ESPLOG_ERROR;
    } else {
        fprintf(stderr, "[log] Unknown LOG_LEVEL '%s', valid values are "
                        "Debug, Info, Warning and Error. Using default (Info)\n", s);
        return;
    }

    esphome_log_set_level(level);
}

void esphome_log_write(esplog_level_t level, const char *fmt, ...) {
    static const char level_char[] = {'D', 'I', 'W', 'E'};
    va_list ap;
    FILE *out;

    if (level < g_esphome_log_level) {
        return;
    }

    out = (level >= ESPLOG_WARNING) ? stderr : stdout;

    va_start(ap, fmt);
    fprintf(out, "[%c] ", level_char[level]);
    vfprintf(out, fmt ? fmt : "\n", ap);
    va_end(ap);
}
