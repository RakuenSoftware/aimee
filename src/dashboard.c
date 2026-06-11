/* dashboard.c: embedded HTTP dashboard server with JSON API endpoints */
#include "aimee.h"
#include "aimee_home.h"
#include "db1.h"
#include "commands.h"
#include "dashboard.h"
#include "kb_client.h"
#include "platform_path.h"
#include "lifecycle.h"
#include "cJSON.h"
#include "headers/agent_exec.h"
#include "headers/memory.h"
#include "headers/plugin.h"
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>

#define DASHBOARD_DEFAULT_PORT 9200
#define DASHBOARD_MAX_REQUEST  8192
#define DASHBOARD_MAX_RESPONSE (256 * 1024)

#define DASHBOARD_MAX_AUDIT_EVENTS 256

typedef struct
{
   char timestamp[32];
   char event[64];
   char detail[512];
   int rc;
   char phase[32];
   char hook[256];
} dashboard_audit_event_t;

static time_t dashboard_parse_utc_timestamp(const char *s)
{
   if (!s || !s[0])
      return (time_t)0;

   int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
   if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &min, &sec) != 6)
      return (time_t)0;

   struct tm tm_buf;
   memset(&tm_buf, 0, sizeof(tm_buf));
   tm_buf.tm_year = year - 1900;
   tm_buf.tm_mon = month - 1;
   tm_buf.tm_mday = day;
   tm_buf.tm_hour = hour;
   tm_buf.tm_min = min;
   tm_buf.tm_sec = sec;
   tm_buf.tm_isdst = 0;
   return timegm(&tm_buf);
}

static int dashboard_parse_rc_value(const char *detail, int *out_rc)
{
   const char *rc = detail ? strstr(detail, "rc=") : NULL;
   if (!rc)
      return 0;
   rc += 3;
   if (!isdigit((unsigned char)*rc) && *rc != '-')
      return 0;
   if (out_rc)
      *out_rc = atoi(rc);
   return 1;
}

static void dashboard_parse_hook_fields(dashboard_audit_event_t *event)
{
   const char *phase = event && event->detail[0] ? strstr(event->detail, "phase=") : NULL;
   const char *hook = event && event->detail[0] ? strstr(event->detail, "hook=") : NULL;

   event->phase[0] = '\0';
   event->hook[0] = '\0';

   if (phase)
   {
      phase += 6;
      size_t len = strcspn(phase, " ");
      if (len >= sizeof(event->phase))
         len = sizeof(event->phase) - 1;
      memcpy(event->phase, phase, len);
      event->phase[len] = '\0';
   }
   if (hook)
      snprintf(event->hook, sizeof(event->hook), "%s", hook + 5);
}

static int dashboard_load_audit_events(const char *filter_event, dashboard_audit_event_t *events,
                                       int max)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/audit.log", config_default_dir());

   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0;

   char line[8192];
   int count = 0;
   while (count < max && fgets(line, sizeof(line), fp))
   {
      cJSON *obj = cJSON_Parse(line);
      if (!obj)
         continue;

      const cJSON *ts = cJSON_GetObjectItemCaseSensitive(obj, "ts");
      const cJSON *ev = cJSON_GetObjectItemCaseSensitive(obj, "event");
      const cJSON *detail = cJSON_GetObjectItemCaseSensitive(obj, "detail");
      if (!cJSON_IsString(ts) || !cJSON_IsString(ev) || !cJSON_IsString(detail))
      {
         cJSON_Delete(obj);
         continue;
      }
      if (filter_event && strcmp(ev->valuestring, filter_event) != 0)
      {
         cJSON_Delete(obj);
         continue;
      }

      dashboard_audit_event_t parsed;
      memset(&parsed, 0, sizeof(parsed));
      snprintf(parsed.timestamp, sizeof(parsed.timestamp), "%s", ts->valuestring);
      snprintf(parsed.event, sizeof(parsed.event), "%s", ev->valuestring);
      snprintf(parsed.detail, sizeof(parsed.detail), "%s", detail->valuestring);
      dashboard_parse_rc_value(parsed.detail, &parsed.rc);
      dashboard_parse_hook_fields(&parsed);

      dashboard_audit_event_t *dst = NULL;
      if (count < max)
      {
         dst = &events[count++];
      }
      else
      {
         memmove(events, events + 1, sizeof(events[0]) * (size_t)(max - 1));
         dst = &events[max - 1];
      }
      memset(dst, 0, sizeof(*dst));
      *dst = parsed;
      cJSON_Delete(obj);
   }

   fclose(fp);
   return count;
}

/* --- Embedded HTML dashboard --- */

/* --- JSON API handlers --- */

/* api_metrics / api_delegations / api_vector_status moved to
 * src/server/dashboard_server.c so they can link into aimee-server (the only
 * caller of those three from the daemon side). The rest of dashboard.c
 * stays here because it reads DB2 directly. */

char *api_token_audit(void)
{
   db1_token_audit_dashboard_row_t rows[100];
   int count = db1_token_audit_list_dashboard(rows, 100);
   if (count < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "tool_name", rows[i].tool_name);
      cJSON_AddStringToObject(obj, "role", rows[i].role);
      cJSON_AddNumberToObject(obj, "prompt_tokens", (double)rows[i].prompt_tokens);
      cJSON_AddNumberToObject(obj, "completion_tokens", (double)rows[i].completion_tokens);
      cJSON_AddNumberToObject(obj, "cache_write_tokens", (double)rows[i].cache_write_tokens);
      cJSON_AddNumberToObject(obj, "cache_read_tokens", (double)rows[i].cache_read_tokens);
      cJSON_AddNumberToObject(obj, "estimated_cost_usd", rows[i].estimated_cost_usd);
      /* REALIZED spend rows (the reader excludes estimated/avoided/partial);
       * declare it so consumers report realized separately (§7). */
      cJSON_AddStringToObject(obj, "usage_kind", "realized");
      cJSON_AddNumberToObject(obj, "call_count", rows[i].call_count);
      cJSON_AddStringToObject(obj, "last_seen", rows[i].last_seen);
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

char *api_traces(void)
{
   db1_execution_trace_tool_call_t rows[100];
   int count = db1_execution_trace_list_tool_calls(rows, 100);
   if (count < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "turn", rows[i].turn);
      cJSON_AddStringToObject(obj, "direction", rows[i].direction);
      if (rows[i].tool_name[0])
         cJSON_AddStringToObject(obj, "tool_name", rows[i].tool_name);
      else
         cJSON_AddNullToObject(obj, "tool_name");
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

/* api_memory_stats moved to src/dashboard_kb.c — see header comment there. */

char *api_plans(void)
{
   db1_execution_plan_summary_t plans[20];
   int count = db1_execution_plan_list_recent_summaries(plans, 20);
   if (count < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "id", plans[i].id);
      cJSON_AddStringToObject(obj, "agent", plans[i].agent_name);
      cJSON_AddStringToObject(obj, "task", plans[i].task);
      cJSON_AddStringToObject(obj, "status", plans[i].status);
      cJSON_AddStringToObject(obj, "created_at", plans[i].created_at);
      cJSON_AddNumberToObject(obj, "total_steps", plans[i].total_steps);
      cJSON_AddNumberToObject(obj, "done_steps", plans[i].done_steps);
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

/* api_logs, api_dashboard_reminders, api_dashboard_recall, api_dashboard_directives
 * moved to src/dashboard_kb.c — see header comment there. */

char *api_dashboard_maintenance(void)
{
   cJSON *obj = cJSON_CreateObject();

   /* Last persisted cycle. */
   memory_maintenance_summary_t last;
   if (memory_maintenance_last_summary(&last) == 0)
   {
      cJSON *j = memory_maintenance_summary_to_json(&last);
      cJSON_AddItemToObject(obj, "last", j);
   }
   else
   {
      cJSON_AddNullToObject(obj, "last");
   }

   /* Process-local totals since boot. */
   int64_t runs_total = 0, skips_total = 0, changes_total = 0;
   double ms_avg = 0.0, ms_max = 0.0;
   memory_maintenance_metrics(&runs_total, &skips_total, &changes_total, &ms_avg, &ms_max);
   cJSON *metrics = cJSON_AddObjectToObject(obj, "metrics");
   cJSON_AddNumberToObject(metrics, "runs_total", (double)runs_total);
   cJSON_AddNumberToObject(metrics, "skips_total", (double)skips_total);
   cJSON_AddNumberToObject(metrics, "changes_total", (double)changes_total);
   cJSON_AddNumberToObject(metrics, "ms_avg", ms_avg);
   cJSON_AddNumberToObject(metrics, "ms_max", ms_max);

   /* Config snapshot so operators can see cadence + gates. */
   config_t cfg;
   config_load(&cfg);
   cJSON *cfg_obj = cJSON_AddObjectToObject(obj, "config");
   cJSON_AddBoolToObject(cfg_obj, "enabled", cfg.memory_maintenance_enabled ? 1 : 0);
   int interval = cfg.memory_maintenance_interval_seconds > 0
                      ? cfg.memory_maintenance_interval_seconds
                      : MEMORY_MAINTENANCE_DEFAULT_INTERVAL_SECS;
   cJSON_AddNumberToObject(cfg_obj, "interval_seconds", interval);
   cJSON_AddBoolToObject(cfg_obj, "summarize_enabled",
                         cfg.memory_maintenance_summarize_enabled ? 1 : 0);

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

char *api_dashboard_identity(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_load(&cfg);
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{}");
   cJSON_AddItemToObject(obj, "charter", identity_charter_json(&cfg));
   cJSON_AddItemToObject(obj, "local_operator", identity_local_operator_json());
   cJSON_AddItemToObject(obj, "working_profile", identity_working_profile_json());
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

/* Dashboard-facing onboard report. Always runs with skip_setup=1
 * because the dashboard surface is read-only — operators who want
 * to re-run setup should invoke `aimee onboard` from the CLI. */
char *api_dashboard_onboard(void)
{
   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   cJSON *report = onboard_build_report(&ctx, /* skip_setup */ 1);
   if (!report)
      return strdup("{}");
   char *json = cJSON_PrintUnformatted(report);
   cJSON_Delete(report);
   return json ? json : strdup("{}");
}

char *api_dashboard_plugins(void)
{
   plugin_t plugins[PLUGIN_MAX_PLUGINS];
   int plugin_count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);

   config_t cfg;
   config_load(&cfg);
   const char *roots[64];
   for (int i = 0; i < cfg.workspace_count && i < 64; i++)
      roots[i] = cfg.workspaces[i];
   plugin_discover_local(roots, cfg.workspace_count, plugins, &plugin_count, PLUGIN_MAX_PLUGINS);

   int enabled = 0;
   int hooks_total = 0;
   int tools_total = 0;
   for (int i = 0; i < plugin_count; i++)
   {
      if (!plugins[i].enabled)
         continue;
      enabled++;
      hooks_total += plugins[i].hook_count;
      tools_total += plugins[i].tool_count;
   }

   dashboard_audit_event_t events[DASHBOARD_MAX_AUDIT_EVENTS];
   int event_count = dashboard_load_audit_events("plugin-hook", events, DASHBOARD_MAX_AUDIT_EVENTS);
   time_t now = time(NULL);
   time_t cutoff = now > 86400 ? now - 86400 : 0;
   int hook_runs_24h = 0;
   int hook_failures_24h = 0;

   cJSON *obj = cJSON_CreateObject();
   cJSON *recent_failures = cJSON_CreateArray();
   if (!obj || !recent_failures)
   {
      cJSON_Delete(obj);
      cJSON_Delete(recent_failures);
      return strdup("{}");
   }

   for (int i = 0; i < event_count; i++)
   {
      time_t ts = dashboard_parse_utc_timestamp(events[i].timestamp);
      if (ts < cutoff)
         continue;
      hook_runs_24h++;
      if (events[i].rc != 0)
         hook_failures_24h++;
   }

   int recent_added = 0;
   for (int i = event_count - 1; i >= 0 && recent_added < 5; i--)
   {
      time_t ts = dashboard_parse_utc_timestamp(events[i].timestamp);
      if (ts < cutoff || events[i].rc == 0)
         continue;
      cJSON *failure = cJSON_CreateObject();
      cJSON_AddStringToObject(failure, "timestamp", events[i].timestamp);
      cJSON_AddStringToObject(failure, "phase", events[i].phase);
      cJSON_AddStringToObject(failure, "hook", events[i].hook);
      cJSON_AddNumberToObject(failure, "rc", events[i].rc);
      cJSON_AddStringToObject(failure, "detail", events[i].detail);
      cJSON_AddItemToArray(recent_failures, failure);
      recent_added++;
   }

   cJSON_AddNumberToObject(obj, "installed", plugin_count);
   cJSON_AddNumberToObject(obj, "enabled", enabled);
   cJSON_AddNumberToObject(obj, "hooks_total", hooks_total);
   cJSON_AddNumberToObject(obj, "tools_total", tools_total);
   cJSON_AddNumberToObject(obj, "hook_runs_24h", hook_runs_24h);
   cJSON_AddNumberToObject(obj, "hook_failures_24h", hook_failures_24h);
   cJSON_AddItemToObject(obj, "recent_failures", recent_failures);

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

char *api_doctor(void)
{
   return doctor_checks_json();
}

char *api_bench_results(void)
{
   /* Read the latest benchmark baseline JSON file */
   const char *base = aimee_home();
   if (!base)
      base = "/root/.config/aimee";

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/benchmarks/baseline.json", base);

   FILE *f = fopen(path, "r");
   if (!f)
   {
      /* Try relative path from working directory */
      f = fopen("benchmarks/baseline.json", "r");
   }
   if (!f)
      return strdup("{\"error\":\"no baseline found\"}");

   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (len <= 0 || len > 1024 * 1024)
   {
      fclose(f);
      return strdup("{\"error\":\"invalid baseline file\"}");
   }

   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return strdup("{\"error\":\"allocation failed\"}");
   }
   size_t nr = fread(buf, 1, (size_t)len, f);
   fclose(f);
   buf[nr] = '\0';
   return buf;
}

/* --- CORS origin management --- */

/* cors_origins/cors_origin_count accessed by posix/dashboard.c */
char cors_origins[CORS_MAX_ORIGINS][CORS_ORIGIN_LEN];
int cors_origin_count = 0;

static const char *cors_file_path(void)
{
   static char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/cors_origins", config_default_dir());
   return path;
}

void cors_load(void)
{
   cors_origin_count = 0;
   FILE *f = fopen(cors_file_path(), "r");
   if (!f)
      return;
   char line[CORS_ORIGIN_LEN];
   while (fgets(line, sizeof(line), f) && cors_origin_count < CORS_MAX_ORIGINS)
   {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';
      if (len > 0)
      {
         snprintf(cors_origins[cors_origin_count], CORS_ORIGIN_LEN, "%s", line);
         cors_origin_count++;
      }
   }
   fclose(f);
}

static void cors_save(void)
{
   FILE *f = fopen(cors_file_path(), "w");
   if (!f)
      return;
   for (int i = 0; i < cors_origin_count; i++)
      fprintf(f, "%s\n", cors_origins[i]);
   fclose(f);
   chmod(cors_file_path(), 0600);
}

int dashboard_cors_add(const char *origin)
{
   cors_load();
   /* Check for duplicate */
   for (int i = 0; i < cors_origin_count; i++)
   {
      if (strcmp(cors_origins[i], origin) == 0)
         return 0; /* already exists */
   }
   if (cors_origin_count >= CORS_MAX_ORIGINS)
      return -1;
   snprintf(cors_origins[cors_origin_count++], CORS_ORIGIN_LEN, "%s", origin);
   cors_save();
   return 0;
}

int dashboard_cors_remove(const char *origin)
{
   cors_load();
   for (int i = 0; i < cors_origin_count; i++)
   {
      if (strcmp(cors_origins[i], origin) == 0)
      {
         for (int j = i; j < cors_origin_count - 1; j++)
            snprintf(cors_origins[j], CORS_ORIGIN_LEN, "%s", cors_origins[j + 1]);
         cors_origin_count--;
         cors_save();
         return 0;
      }
   }
   return -1; /* not found */
}

int dashboard_cors_list(char origins[][CORS_ORIGIN_LEN], int max)
{
   cors_load();
   int count = cors_origin_count < max ? cors_origin_count : max;
   for (int i = 0; i < count; i++)
      snprintf(origins[i], CORS_ORIGIN_LEN, "%s", cors_origins[i]);
   return count;
}

/* dashboard_serve() is implemented in posix/dashboard.c (Linux/macOS)
 * and windows/dashboard.c (Windows). */
