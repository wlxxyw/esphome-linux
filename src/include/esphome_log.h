/**
 * @file esphome_log.h
 * @brief Minimal global leveled logging (Debug/Info/Warning/Error)
 *
 * Usage: replace printf/fprintf calls with LOGI/LOGE (etc.) macros.
 * Output format is unchanged apart from a level tag ("[D] [I] [W] [E] ");
 * Warning/Error go to stderr, Debug/Info go to stdout (same as before).
 *
 * Global level is initialized by esphome_log_init() from the LOG_LEVEL
 * environment variable (Debug/Info/Warning/Error, case-insensitive,
 * default: Info), and can be changed at runtime with esphome_log_set_level().
 */

#ifndef ESPHOME_LOG_H
#define ESPHOME_LOG_H

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPLOG_DEBUG   = 0,
    ESPLOG_INFO    = 1,
    ESPLOG_WARNING = 2,
    ESPLOG_ERROR   = 3
} esplog_level_t;

/**
 * Parse the LOG_LEVEL environment variable and set the global level.
 * Safe to call more than once; defaults to Info if unset/invalid.
 */
void esphome_log_init(void);

/** Set the global log level at runtime. */
void esphome_log_set_level(esplog_level_t level);

/** Get the current global log level. */
esplog_level_t esphome_log_get_level(void);

/**
 * Core logging function. Messages below the global level are dropped.
 */
void esphome_log_write(esplog_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

/** @cond */

#define LOGD(...) esphome_log_write(ESPLOG_DEBUG, __VA_ARGS__)
#define LOGI(...) esphome_log_write(ESPLOG_INFO, __VA_ARGS__)
#define LOGW(...) esphome_log_write(ESPLOG_WARNING, __VA_ARGS__)
#define LOGE(...) esphome_log_write(ESPLOG_ERROR, __VA_ARGS__)

/** @endcond */

#endif /* ESPHOME_LOG_H */
