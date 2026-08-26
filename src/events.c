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

static const char *notify_config_path(void)
{
   static char path[MAX_PATH_LEN];
   static char cached_dir[MAX_PATH_LEN];
   const char *dir = config_default_dir();

   if (path[0] && strcmp(cached_dir, dir) == 0)
      return path;

   snprintf(path, sizeof(path), "%s/notifications.json", dir);
   snprintf(cached_dir, sizeof(cached_dir), "%s", dir);
   return path;
}

static void notify_ensure_config_dir(void)
{
   const char *dir = config_default_dir();
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s", dir);

   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         platform_mkdir_p(tmp, 0700);
         *p = '/';
      }
   }
   platform_mkdir_p(tmp, 0700);
}

static int event_name_to_type(const char *name)
{
   int i;

   if (!name || !name[0])
      return -1;

   for (i = 0; i < AIMEE_EVENT_COUNT; i++)
   {
      if (strcmp(name, event_type_name((aimee_event_t)i)) == 0)
         return i;
   }
   return -1;
}

static const char *verbosity_name(int verbosity)
{
   switch (verbosity)
   {
   case NOTIFY_VERBOSITY_MINIMAL:
      return "minimal";
   case NOTIFY_VERBOSITY_STANDARD:
      return "standard";
   case NOTIFY_VERBOSITY_VERBOSE:
      return "verbose";
   default:
      return "standard";
   }
}

static int verbosity_from_string(const char *name)
{
   if (!name || !name[0])
      return NOTIFY_VERBOSITY_STANDARD;
   if (strcmp(name, "minimal") == 0)
      return NOTIFY_VERBOSITY_MINIMAL;
   if (strcmp(name, "verbose") == 0)
      return NOTIFY_VERBOSITY_VERBOSE;
   return NOTIFY_VERBOSITY_STANDARD;
}

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

static void substitute_template(char *dst, size_t dst_len, const char *tmpl, const char *event_name,
                                const char *message)
{
   const char *p = tmpl ? tmpl : "";
   size_t pos = 0;

   if (!dst || dst_len == 0)
      return;

   while (*p && pos + 1 < dst_len)
   {
      if (pos >= dst_len - 1)
         break;
      if (strncmp(p, "{{message}}", 11) == 0)
      {
         append_str(dst, dst_len, &pos, message ? message : "");
         p += 11;
         continue;
      }
      if (strncmp(p, "{{event}}", 9) == 0)
      {
         append_str(dst, dst_len, &pos, event_name ? event_name : "");
         p += 9;
         continue;
      }
      dst[pos++] = *p++;
   }

   dst[pos] = '\0';
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

static int target_matches_event(const notify_target_t *target, aimee_event_t event)
{
   unsigned int bit;

   if (!target)
      return 0;
   if (target->event_mask == 0)
      return 1;
   if (event < 0 || event >= AIMEE_EVENT_COUNT)
      return 0;

   bit = 1u << (unsigned int)event;
   return (target->event_mask & bit) != 0;
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

/* Platform-specific async notification delivery (posix/events.c, windows/events.c) */
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

int notify_config_load(notify_config_t *cfg)
{
   const char *path = notify_config_path();
   FILE *fp;
   long len;
   char *buf;
   size_t nread;
   cJSON *root;
   cJSON *item;
   cJSON *targets;
   cJSON *target;
   int idx;

   if (!cfg)
      return -1;

   memset(cfg, 0, sizeof(*cfg));
   cfg->verbosity = NOTIFY_VERBOSITY_STANDARD;

   fp = fopen(path, "r");
   if (!fp)
   {
      if (errno == ENOENT)
         return 0;
      aimee_log(LOG_ERROR, "notify", "failed to open %s: %s", path, strerror(errno));
      return -1;
   }

   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      aimee_log(LOG_ERROR, "notify", "failed to seek %s", path);
      return -1;
   }
   len = ftell(fp);
   if (len < 0)
   {
      fclose(fp);
      aimee_log(LOG_ERROR, "notify", "failed to size %s", path);
      return -1;
   }
   if (fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      aimee_log(LOG_ERROR, "notify", "failed to rewind %s", path);
      return -1;
   }

   if (len == 0)
   {
      fclose(fp);
      return 0;
   }
   if (len > MAX_FILE_SIZE)
   {
      fclose(fp);
      aimee_log(LOG_ERROR, "notify", "config too large: %s", path);
      return -1;
   }

   buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(fp);
      aimee_log(LOG_ERROR, "notify", "out of memory loading %s", path);
      return -1;
   }

   nread = fread(buf, 1, (size_t)len, fp);
   fclose(fp);
   buf[nread] = '\0';

   root = cJSON_Parse(buf);
   free(buf);
   if (!root)
   {
      aimee_log(LOG_ERROR, "notify", "failed to parse %s", path);
      return -1;
   }

   item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
   if (cJSON_IsBool(item))
      cfg->enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "verbosity");
   if (cJSON_IsString(item) && item->valuestring)
      cfg->verbosity = verbosity_from_string(item->valuestring);

   targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
   if (cJSON_IsArray(targets))
   {
      idx = 0;
      cJSON_ArrayForEach(target, targets)
      {
         cJSON *type_item;
         cJSON *cmd_item;
         cJSON *url_item;
         cJSON *target_item;
         cJSON *base_url_item;
         cJSON *events_item;
         cJSON *event_item;
         notify_target_t *dst;
         int valid = 0;

         if (idx >= NOTIFY_MAX_TARGETS)
            break;
         if (!cJSON_IsObject(target))
            continue;

         dst = &cfg->targets[idx];
         memset(dst, 0, sizeof(*dst));

         type_item = cJSON_GetObjectItemCaseSensitive(target, "type");
         if (!cJSON_IsString(type_item) || !type_item->valuestring)
            continue;

         if (strcmp(type_item->valuestring, "webhook") == 0)
         {
            dst->is_webhook = 1;
         }
         else if (strcmp(type_item->valuestring, "command") == 0)
         {
            dst->is_webhook = 0;
         }
         else if (strcmp(type_item->valuestring, "ntfy") == 0)
         {
            dst->is_delivery = 1;
            target_item = cJSON_GetObjectItemCaseSensitive(target, "target");
            if (cJSON_IsString(target_item) && target_item->valuestring[0] &&
                delivery_target_parse(target_item->valuestring, &dst->target) == 0 &&
                notify_target_is_ntfy(dst))
            {
               base_url_item = cJSON_GetObjectItemCaseSensitive(target, "base_url");
               if (!cJSON_IsString(base_url_item) || !base_url_item->valuestring[0])
                  base_url_item = cJSON_GetObjectItemCaseSensitive(target, "url");
               if (cJSON_IsString(base_url_item) && base_url_item->valuestring[0])
                  snprintf(dst->base_url, sizeof(dst->base_url), "%s", base_url_item->valuestring);
               else
                  snprintf(dst->base_url, sizeof(dst->base_url), "%s", NTFY_DEFAULT_BASE_URL);
               valid = 1;
            }
            else
            {
               aimee_log(LOG_WARN, "notify", "invalid ntfy notification target, skipping");
            }
         }
         else if (strcmp(type_item->valuestring, "local") == 0)
         {
            const char *spec = "local";
            dst->is_delivery = 1;
            target_item = cJSON_GetObjectItemCaseSensitive(target, "target");
            if (cJSON_IsString(target_item) && target_item->valuestring[0])
               spec = target_item->valuestring;
            if (delivery_target_parse(spec, &dst->target) == 0 && notify_target_is_local(dst))
               valid = 1;
            else
               aimee_log(LOG_WARN, "notify", "invalid local notification target, skipping");
         }
         else
         {
            aimee_log(LOG_WARN, "notify", "unknown notification target type '%s', skipping",
                      type_item->valuestring);
            continue;
         }

         if (dst->is_delivery)
         {
            /* Already validated by the typed delivery branch above. */
         }
         else if (dst->is_webhook)
         {
            url_item = cJSON_GetObjectItemCaseSensitive(target, "url");
            if (cJSON_IsString(url_item) && url_item->valuestring[0])
            {
               snprintf(dst->url, sizeof(dst->url), "%s", url_item->valuestring);
               valid = 1;
            }
         }
         else
         {
            cmd_item = cJSON_GetObjectItemCaseSensitive(target, "command");
            if (cJSON_IsString(cmd_item) && cmd_item->valuestring[0])
            {
               snprintf(dst->command, sizeof(dst->command), "%s", cmd_item->valuestring);
               valid = 1;
            }
         }

         if (!valid)
            continue;

         events_item = cJSON_GetObjectItemCaseSensitive(target, "events");
         if (cJSON_IsArray(events_item))
         {
            unsigned int mask = 0;
            cJSON_ArrayForEach(event_item, events_item)
            {
               int ev;
               if (!cJSON_IsString(event_item) || !event_item->valuestring)
                  continue;
               ev = event_name_to_type(event_item->valuestring);
               if (ev >= 0)
                  mask |= 1u << (unsigned int)ev;
            }
            dst->event_mask = mask;
         }

         idx++;
      }
      cfg->target_count = idx;
   }

   cJSON_Delete(root);
   return 0;
}

int notify_config_save(const notify_config_t *cfg)
{
   cJSON *root;
   cJSON *targets;
   char *json_str;
   char path[MAX_PATH_LEN];
   char tmp[MAX_PATH_LEN];
   FILE *fp;
   int i;

   if (!cfg)
      return -1;

   notify_ensure_config_dir();

   root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON_AddBoolToObject(root, "enabled", cfg->enabled ? 1 : 0);
   cJSON_AddStringToObject(root, "verbosity", verbosity_name(cfg->verbosity));
   targets = cJSON_AddArrayToObject(root, "targets");

   for (i = 0; i < cfg->target_count && i < NOTIFY_MAX_TARGETS; i++)
   {
      cJSON *obj;
      cJSON *events;
      int ev;
      const notify_target_t *target = &cfg->targets[i];

      obj = cJSON_CreateObject();
      if (!obj)
         continue;

      if (target->is_delivery)
      {
         char spec[256];
         if (notify_target_is_ntfy(target))
         {
            if (delivery_target_format(&target->target, spec, sizeof(spec)) != 0)
            {
               cJSON_Delete(obj);
               continue;
            }
            cJSON_AddStringToObject(obj, "type", "ntfy");
            cJSON_AddStringToObject(obj, "target", spec);
            if (target->base_url[0])
               cJSON_AddStringToObject(obj, "base_url", target->base_url);
         }
         else if (notify_target_is_local(target))
         {
            if (delivery_target_format(&target->target, spec, sizeof(spec)) != 0)
            {
               cJSON_Delete(obj);
               continue;
            }
            cJSON_AddStringToObject(obj, "type", "local");
            cJSON_AddStringToObject(obj, "target", spec);
         }
         else
         {
            cJSON_Delete(obj);
            continue;
         }
      }
      else if (target->is_webhook)
      {
         cJSON_AddStringToObject(obj, "type", "webhook");
         if (target->url[0])
            cJSON_AddStringToObject(obj, "url", target->url);
      }
      else
      {
         cJSON_AddStringToObject(obj, "type", "command");
         if (target->command[0])
            cJSON_AddStringToObject(obj, "command", target->command);
      }

      if (target->event_mask != 0)
      {
         events = cJSON_AddArrayToObject(obj, "events");
         for (ev = 0; ev < AIMEE_EVENT_COUNT; ev++)
         {
            unsigned int bit = 1u << (unsigned int)ev;
            if (target->event_mask & bit)
               cJSON_AddItemToArray(events, cJSON_CreateString(event_type_name((aimee_event_t)ev)));
         }
      }

      cJSON_AddItemToArray(targets, obj);
   }

   json_str = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json_str)
      return -1;

   snprintf(path, sizeof(path), "%s", notify_config_path());
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

   fp = fopen(tmp, "w");
   if (!fp)
   {
      aimee_log(LOG_ERROR, "notify", "failed to open %s: %s", tmp, strerror(errno));
      free(json_str);
      return -1;
   }

   fputs(json_str, fp);
   fputc('\n', fp);
   fclose(fp);
   free(json_str);

   chmod(tmp, 0600);

   if (rename(tmp, path) != 0)
   {
      aimee_log(LOG_ERROR, "notify", "rename failed for %s: %s", path, strerror(errno));
      unlink(tmp);
      return -1;
   }

   return 0;
}

void event_notify(aimee_event_t event, const char *message)
{
   notify_config_t cfg;
   const char *event_name;
   const char *msg;
   int i;

   if (notify_config_load(&cfg) != 0)
      return;

   if (!cfg.enabled)
      return;
   if (!event_matches_verbosity(event, cfg.verbosity))
      return;

   event_name = event_type_name(event);
   msg = message ? message : "";

   for (i = 0; i < cfg.target_count && i < NOTIFY_MAX_TARGETS; i++)
   {
      const notify_target_t *target = &cfg.targets[i];

      if (!target_matches_event(target, event))
         continue;

      if (target->is_delivery)
      {
         if (notify_deliver_target(target, event_name, msg) != 0)
            aimee_log(LOG_WARN, "notify", "failed to deliver typed notification target for %s",
                      event_name);
      }
      else if (target->is_webhook)
      {
         char event_json[128];
         char msg_json[2048];
         char payload[2304];
         char cmd[8192];

         json_escape(event_json, sizeof(event_json), event_name);
         json_escape(msg_json, sizeof(msg_json), msg);
         snprintf(payload, sizeof(payload), "{\"event\":\"%s\",\"message\":\"%s\"}", event_json,
                  msg_json);
         char *payload_quoted = shell_quote(payload);
         char *url_quoted = shell_quote(target->url);
         snprintf(cmd, sizeof(cmd), "curl -s -X POST -H 'Content-Type: application/json' -d %s %s",
                  payload_quoted, url_quoted);
         free(payload_quoted);
         free(url_quoted);
         deliver_detached_shell(cmd);
      }
      else
      {
         char cmd[1024];
         substitute_template(cmd, sizeof(cmd), target->command, event_name, msg);
         deliver_detached_shell(cmd);
      }
   }
}

const char *event_type_name(aimee_event_t event)
{
   switch (event)
   {
   case AIMEE_EVENT_DELEGATE_COMPLETE:
      return "delegate_complete";
   case AIMEE_EVENT_DELEGATE_FAILED:
      return "delegate_failed";
   case AIMEE_EVENT_VERIFY_PASS:
      return "verify_pass";
   case AIMEE_EVENT_VERIFY_FAIL:
      return "verify_fail";
   case AIMEE_EVENT_JOB_COMPLETE:
      return "job_complete";
   case AIMEE_EVENT_SESSION_IDLE:
      return "session_idle";
   default:
      return "unknown";
   }
}

int event_matches_verbosity(aimee_event_t event, int verbosity)
{
   if (verbosity >= NOTIFY_VERBOSITY_VERBOSE)
      return 1;

   switch (event)
   {
   case AIMEE_EVENT_DELEGATE_FAILED:
   case AIMEE_EVENT_VERIFY_FAIL:
   case AIMEE_EVENT_JOB_COMPLETE:
   case AIMEE_EVENT_SESSION_IDLE:
      return 1;
   case AIMEE_EVENT_DELEGATE_COMPLETE:
   case AIMEE_EVENT_VERIFY_PASS:
      return verbosity >= NOTIFY_VERBOSITY_STANDARD;
   default:
      return 0;
   }
}
