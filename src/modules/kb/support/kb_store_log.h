#ifndef AIMEE_KB_STORE_LOG_H
#define AIMEE_KB_STORE_LOG_H

#include <stdarg.h>

typedef enum
{
   LOG_ERROR = 0,
   LOG_WARN = 1,
   LOG_INFO = 2,
   LOG_DEBUG = 3
} log_level_t;

typedef void (*kb_store_log_sink_fn)(void *context, log_level_t level, const char *module,
                                     const char *message);

/* Install once during process startup, before worker threads begin dispatch. */
void kb_store_log_install(kb_store_log_sink_fn sink, void *context);

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_ERROR(mod, ...) aimee_log(LOG_ERROR, mod, __VA_ARGS__)
#define LOG_WARN(mod, ...)  aimee_log(LOG_WARN, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...)  aimee_log(LOG_INFO, mod, __VA_ARGS__)
#define LOG_DEBUG(mod, ...) aimee_log(LOG_DEBUG, mod, __VA_ARGS__)

#endif
