/* events.c: event notification hooks (fire-and-forget delivery to command/webhook targets) */
#include "aimee.h"
#include "platform_path.h"
#include "config.h"
#include "events.h"
#include "log.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define NTFY_DEFAULT_BASE_URL "https://ntfy.sh"
#define LOCAL_NOTIFY_REL_DIR  ".cache/aimee/notifications"
#define LOCAL_NOTIFY_FILE     "events.jsonl"

static void append_str(char *dst, size_t dst_len, size_t *pos, const char *src)
{
   size_t rem;
   int n;

   if (!dst || !dst_len || !pos || !src)
      return;
   if (*pos >= dst_len)
      return;

   rem = dst_len - *pos;
   n = snprintf(dst + *pos, rem, "%s", src);
   if (n < 0)
      return;
   if ((size_t)n >= rem)
      *pos = dst_len - 1;
   else
      *pos += (size_t)n;
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
   size_t pos = 0;
   const unsigned char *p = (const unsigned char *)(src ? src : "");

   if (!dst || dst_len == 0)
      return;

   while (*p && pos + 1 < dst_len)
   {
      switch (*p)
      {
      case '\\':
         append_str(dst, dst_len, &pos, "\\\\");
         break;
      case '"':
         append_str(dst, dst_len, &pos, "\\\"");
         break;
      case '\n':
         append_str(dst, dst_len, &pos, "\\n");
         break;
      case '\r':
         append_str(dst, dst_len, &pos, "\\r");
         break;
      case '\t':
         append_str(dst, dst_len, &pos, "\\t");
         break;
      default:
         if (*p < 0x20)
         {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)*p);
            append_str(dst, dst_len, &pos, tmp);
         }
         else
         {
            dst[pos++] = (char)*p;
         }
         break;
      }
      p++;
   }

   if (pos >= dst_len)
      pos = dst_len - 1;
   dst[pos] = '\0';
}

static int ntfy_topic_valid(const char *topic)
{
   if (!topic || !topic[0])
      return 0;
   for (const char *p = topic; *p; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (!(isalnum(c) || c == '_' || c == '-' || c == '.'))
         return 0;
   }
   return 1;
}

static int notify_target_is_ntfy(const notify_target_t *target)
{
   return target && target->is_delivery && strcmp(target->target.platform, "ntfy") == 0 &&
          ntfy_topic_valid(target->target.chat_id) && !target->target.thread_id[0];
}

static int notify_target_is_local(const notify_target_t *target)
{
   return target && target->is_delivery && strcmp(target->target.platform, "local") == 0 &&
          !target->target.chat_id[0] && !target->target.thread_id[0];
}

static int ntfy_build_url(const notify_target_t *target, char *buf, size_t bufsz)
{
   if (!notify_target_is_ntfy(target) || !buf || bufsz == 0)
      return -1;

   const char *base = target->base_url[0] ? target->base_url : NTFY_DEFAULT_BASE_URL;
   size_t base_len = strlen(base);
   while (base_len > 0 && base[base_len - 1] == '/')
      base_len--;
   if (base_len == 0)
      return -1;

   int n = snprintf(buf, bufsz, "%.*s/%s", (int)base_len, base, target->target.chat_id);
   return (n > 0 && (size_t)n < bufsz) ? 0 : -1;
}

int notify_build_ntfy_command(const notify_target_t *target, const char *event_name,
                              const char *message, char *cmd, size_t cmd_len)
{
   if (!cmd || cmd_len == 0)
      return -1;
   cmd[0] = '\0';
   if (!notify_target_is_ntfy(target))
      return -1;

   char url[768];
   char title_header[192];
   if (ntfy_build_url(target, url, sizeof(url)) != 0)
      return -1;
   snprintf(title_header, sizeof(title_header), "Title: aimee: %s",
            event_name && event_name[0] ? event_name : "event");

   char *url_quoted = shell_quote(url);
   char *title_quoted = shell_quote(title_header);
   char *msg_quoted = shell_quote(message ? message : "");
   int n = snprintf(cmd, cmd_len,
                    "curl -s -X POST -H %s -H 'Priority: default' -H 'Tags: aimee' -d %s %s",
                    title_quoted, msg_quoted, url_quoted);
   free(url_quoted);
   free(title_quoted);
   free(msg_quoted);
   return (n > 0 && (size_t)n < cmd_len) ? 0 : -1;
}

static int notify_local_paths(char *dir, size_t dir_len, char *path, size_t path_len)
{
   const char *home = platform_home_dir();
   if (!home || !home[0] || !dir || dir_len == 0 || !path || path_len == 0)
      return -1;

   int dn = snprintf(dir, dir_len, "%s/%s", home, LOCAL_NOTIFY_REL_DIR);
   int pn = snprintf(path, path_len, "%s/%s/%s", home, LOCAL_NOTIFY_REL_DIR, LOCAL_NOTIFY_FILE);
   return (dn > 0 && (size_t)dn < dir_len && pn > 0 && (size_t)pn < path_len) ? 0 : -1;
}

static int notify_deliver_local(const char *event_name, const char *message)
{
   char dir[MAX_PATH_LEN];
   char path[MAX_PATH_LEN];
   char event_json[128];
   char msg_json[2048];

   if (notify_local_paths(dir, sizeof(dir), path, sizeof(path)) != 0)
      return -1;
   if (platform_mkdir_p(dir, 0700) != 0)
      return -1;

   FILE *fp = fopen(path, "a");
   if (!fp)
      return -1;

   json_escape(event_json, sizeof(event_json), event_name);
   json_escape(msg_json, sizeof(msg_json), message);
   fprintf(fp, "{\"event\":\"%s\",\"message\":\"%s\"}\n", event_json, msg_json);
   fclose(fp);
   chmod(path, 0600);
   return 0;
}

/* Platform-specific async notification delivery (posix/events.c) */
void platform_events_deliver_detached(const char *cmd);

static void deliver_detached_shell(const char *cmd)
{
   if (!cmd || !cmd[0])
      return;

   platform_events_deliver_detached(cmd);
}

int notify_deliver_target(const notify_target_t *target, const char *event_name,
                          const char *message)
{
   if (!target || !target->is_delivery)
      return -1;

   if (notify_target_is_ntfy(target))
   {
      char cmd[8192];
      if (notify_build_ntfy_command(target, event_name, message, cmd, sizeof(cmd)) != 0)
         return -1;
      deliver_detached_shell(cmd);
      return 0;
   }

   if (notify_target_is_local(target))
      return notify_deliver_local(event_name, message);

   return -1;
}
