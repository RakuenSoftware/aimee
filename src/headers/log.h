#ifndef DEC_LOG_H
#define DEC_LOG_H 1

#include <stdarg.h>

typedef enum
{
   LOG_ERROR = 0,
   LOG_WARN = 1,
   LOG_INFO = 2,
   LOG_DEBUG = 3
} log_level_t;

/* Initialize logging. Call once at startup. */
void log_init(log_level_t level);

/* Set the global log level at runtime. */
void log_set_level(log_level_t level);

/* Get the current log level. */
log_level_t log_get_level(void);

/* Parse a log level string ("error", "warn", "info", "debug"). Returns -1 on invalid. */
int log_parse_level(const char *str, log_level_t *out);

/* Core logging function. Writes to stderr with timestamp, level, and module. */
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Security audit event. Always logged regardless of log level.
 * Written to both stderr and the audit log file. */
void audit_log(const char *event_type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Governed-action audit row (per-tool-call). Writes a single structured line
 * {"ts","kind":"tool_action","actor","tool","args_hash","command","mode",
 * "reason_code","verdict","task_id"} to the SAME audit.log as audit_log (single
 * sink), sharing its mutex + rotation. Distinguished from audit_log rows by the
 * top-level "kind" key. `command` is the arg-free human-readable command preview
 * (see audit_command_preview) — never an argument value. Best-effort/
 * side-effect-only: callers use this off the enforcement path and never gate on
 * it. Any string arg may be NULL (rendered as ""). */
void audit_action_log(const char *actor, const char *tool, const char *args_hash,
                      const char *command, const char *mode, const char *reason_code,
                      const char *verdict, long long task_id);

/* Last audit event key set by audit_log() on this thread (empty if none since
 * the last reset). Used to derive a governed-action reason_code. */
const char *audit_last_event(void);
void audit_last_event_reset(void);

/* Convenience macros */
#define LOG_ERROR(mod, ...) aimee_log(LOG_ERROR, mod, __VA_ARGS__)
#define LOG_WARN(mod, ...)  aimee_log(LOG_WARN, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...)  aimee_log(LOG_INFO, mod, __VA_ARGS__)
#define LOG_DEBUG(mod, ...) aimee_log(LOG_DEBUG, mod, __VA_ARGS__)

/* Audit log file management */
void audit_log_open(void);
void audit_log_close(void);

#endif /* DEC_LOG_H */
