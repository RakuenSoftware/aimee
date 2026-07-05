/* log.c: structured logging with levels and security audit trail */
#include "log.h"
#include "platform_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

/* Forward declaration to avoid circular header dependency */
const char *config_default_dir(void);

static log_level_t global_level = LOG_INFO;
static FILE *audit_fp = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

#define AUDIT_MAX_SIZE  (10 * 1024 * 1024) /* 10MB */
#define AUDIT_MAX_FILES 5

static const char *level_names[] = {"ERROR", "WARN", "INFO", "DEBUG"};

void log_init(log_level_t level)
{
   global_level = level;

   /* Check environment override */
   const char *env = getenv("AIMEE_LOG_LEVEL");
   if (env)
   {
      log_level_t parsed;
      if (log_parse_level(env, &parsed) == 0)
         global_level = parsed;
   }
}

void log_set_level(log_level_t level)
{
   global_level = level;
}

log_level_t log_get_level(void)
{
   return global_level;
}

int log_parse_level(const char *str, log_level_t *out)
{
   if (!str || !out)
      return -1;
   if (strcasecmp(str, "error") == 0)
      *out = LOG_ERROR;
   else if (strcasecmp(str, "warn") == 0)
      *out = LOG_WARN;
   else if (strcasecmp(str, "info") == 0)
      *out = LOG_INFO;
   else if (strcasecmp(str, "debug") == 0)
      *out = LOG_DEBUG;
   else
      return -1;
   return 0;
}

static void format_timestamp(char *buf, size_t len)
{
   time_t t = time(NULL);
   struct tm tm_buf;
   gmtime_r(&t, &tm_buf);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   if (level > global_level)
      return;

   char ts[32];
   format_timestamp(ts, sizeof(ts));

   pthread_mutex_lock(&log_mutex);

   fprintf(stderr, "%s %-5s %s: ", ts, level_names[level], module ? module : "aimee");

   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);

   fprintf(stderr, "\n");

   pthread_mutex_unlock(&log_mutex);
}

/* Rotate audit log if it exceeds size limit */
static void audit_rotate(const char *path)
{
   struct stat st;
   if (stat(path, &st) != 0 || st.st_size < AUDIT_MAX_SIZE)
      return;

   /* Rotate: audit.log.4 -> deleted, .3 -> .4, .2 -> .3, .1 -> .2, .0 -> .1, current -> .0 */
   char old_path[4096], new_path[4096];
   for (int i = AUDIT_MAX_FILES - 1; i >= 0; i--)
   {
      if (i == 0)
         snprintf(old_path, sizeof(old_path), "%s", path);
      else
         snprintf(old_path, sizeof(old_path), "%s.%d", path, i - 1);

      snprintf(new_path, sizeof(new_path), "%s.%d", path, i);

      if (i == AUDIT_MAX_FILES - 1)
         platform_unlink(new_path);
      rename(old_path, new_path);
   }
}

/* Last audit event key on this thread. Lets pre_tool_check derive a stable
 * reason_code from a block site's existing audit_log("<key>",...) call without
 * threading extra state through the guardrail function. */
static __thread char g_last_audit_event[48];
void audit_last_event_reset(void)
{
   g_last_audit_event[0] = '\0';
}
const char *audit_last_event(void)
{
   return g_last_audit_event;
}

void audit_log_open(void)
{
   if (audit_fp)
      return;

   char path[4096];
   snprintf(path, sizeof(path), "%s/audit.log", config_default_dir());

   audit_rotate(path);

   audit_fp = fopen(path, "a");
   if (audit_fp)
      platform_set_permissions(path, 0600);
}

void audit_log_close(void)
{
   if (audit_fp)
   {
      fclose(audit_fp);
      audit_fp = NULL;
   }
}

/* JSON-escape src into dst (bounded), RFC 8259 §7 compliant for control bytes so
 * a model-controlled field (e.g. tool_name) cannot emit an invalid audit line.
 * Reserves 6 bytes per iteration for the worst case (\uXXXX). */
static void audit_json_escape(char *dst, size_t dstsz, const char *src)
{
   size_t ep = 0;
   if (!src)
      src = "";
   for (size_t i = 0; src[i] && ep + 6 < dstsz; i++)
   {
      unsigned char c = (unsigned char)src[i];
      if (c == '"' || c == '\\')
      {
         dst[ep++] = '\\';
         dst[ep++] = (char)c;
      }
      else if (c == '\n')
      {
         dst[ep++] = '\\';
         dst[ep++] = 'n';
      }
      else if (c == '\r')
      {
         dst[ep++] = '\\';
         dst[ep++] = 'r';
      }
      else if (c == '\t')
      {
         dst[ep++] = '\\';
         dst[ep++] = 't';
      }
      else if (c < 0x20)
      {
         ep += (size_t)snprintf(dst + ep, dstsz - ep, "\\u%04x", c);
      }
      else
      {
         dst[ep++] = (char)c;
      }
   }
   dst[ep] = '\0';
}

/* Rotate audit.log if it has reached AUDIT_MAX_SIZE. Caller MUST hold log_mutex.
 * Single source of truth for both audit_log and audit_action_log. */
static void audit_maybe_rotate_locked(void)
{
   if (!audit_fp)
      return;
   char path[4096];
   snprintf(path, sizeof(path), "%s/audit.log", config_default_dir());
   struct stat st;
   if (stat(path, &st) == 0 && st.st_size >= AUDIT_MAX_SIZE)
   {
      fclose(audit_fp);
      audit_fp = NULL;
      audit_rotate(path);
      audit_fp = fopen(path, "a");
      if (audit_fp)
         platform_set_permissions(path, 0600);
   }
}

void audit_action_log(const char *actor, const char *tool, const char *args_hash,
                      const char *command, const char *mode, const char *reason_code,
                      const char *verdict, long long task_id)
{
   char ts[32];
   format_timestamp(ts, sizeof(ts));

   char e_actor[128], e_tool[128], e_hash[96], e_cmd[320], e_mode[64], e_reason[96], e_verdict[32];
   audit_json_escape(e_actor, sizeof e_actor, actor);
   audit_json_escape(e_tool, sizeof e_tool, tool);
   audit_json_escape(e_hash, sizeof e_hash, args_hash);
   audit_json_escape(e_cmd, sizeof e_cmd, command);
   audit_json_escape(e_mode, sizeof e_mode, mode);
   audit_json_escape(e_reason, sizeof e_reason, reason_code);
   audit_json_escape(e_verdict, sizeof e_verdict, verdict);

   /* No stderr mirror: tool_action fires on every governed call and would flood
    * the server log. The row goes only to the audit file. */
   pthread_mutex_lock(&log_mutex);

   if (audit_fp)
   {
      fprintf(audit_fp,
              "{\"ts\":\"%s\",\"kind\":\"tool_action\",\"actor\":\"%s\",\"tool\":\"%s\","
              "\"args_hash\":\"%s\",\"command\":\"%s\",\"mode\":\"%s\",\"reason_code\":\"%s\","
              "\"verdict\":\"%s\",\"task_id\":%lld}\n",
              ts, e_actor, e_tool, e_hash, e_cmd, e_mode, e_reason, e_verdict, task_id);
      fflush(audit_fp);
      audit_maybe_rotate_locked();
   }

   pthread_mutex_unlock(&log_mutex);
}

void audit_log(const char *event_type, const char *fmt, ...)
{
   snprintf(g_last_audit_event, sizeof g_last_audit_event, "%s", event_type ? event_type : "");
   char ts[32];
   format_timestamp(ts, sizeof(ts));

   char message[2048];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(message, sizeof(message), fmt, ap);
   va_end(ap);

   pthread_mutex_lock(&log_mutex);

   /* Always write to stderr */
   fprintf(stderr, "%s AUDIT %s: %s\n", ts, event_type, message);

   /* Write to audit file as JSON line */
   if (audit_fp)
   {
      /* Escape message for JSON */
      char escaped[4096];
      size_t ep = 0;
      for (size_t i = 0; message[i] && ep < sizeof(escaped) - 2; i++)
      {
         if (message[i] == '"' || message[i] == '\\')
            escaped[ep++] = '\\';
         if (message[i] == '\n')
         {
            escaped[ep++] = '\\';
            escaped[ep++] = 'n';
         }
         else
         {
            escaped[ep++] = message[i];
         }
      }
      escaped[ep] = '\0';

      fprintf(audit_fp, "{\"ts\":\"%s\",\"event\":\"%s\",\"detail\":\"%s\"}\n", ts, event_type,
              escaped);
      fflush(audit_fp);
      audit_maybe_rotate_locked();
   }

   pthread_mutex_unlock(&log_mutex);
}
