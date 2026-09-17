#include "json_fluent.h"
#include "module_commands.h"
#include "aimee/memory/module_api.h"
/* dashboard_kb.c: dashboard JSON helpers that touch only DB2 + audit.log.
 *
 * These are the dashboard endpoints the kb sidecar's request handlers
 * (db2/kb_service_backend_agent.c → db2_kb_service_dashboard_*) call
 * directly when the daemon proxies "dashboard.*" RPCs through to kb.
 * They live here, not in dashboard.c, so the $(KB) link rule can pull
 * them in without dragging the rest of dashboard.c (api_doctor, cors_*,
 * etc.) — that file stays in CMD_OBJS and links
 * into the daemon-adjacent binaries.
 *
 * Note on api_logs: the kb-side version intentionally omits the DB1
 * agent_log rows. aimee-kb is being pinned to DB2 only (architecture
 * lock rule 2), so reaching into db1_agent_log_* from this file would
 * violate that boundary. The daemon-side dashboard handler is free to
 * splice agent_log rows into the response if it ever needs to. */
#include "aimee.h"
#include "cJSON.h"
#include "modules/db2/c/decision_log.h"
#include "modules/db2/c/memory_conflicts.h"
#include "modules/db2/c/memory_query.h"
#include <aimee/memory/module_api.h>
#include "dashboard.h"
#include "lifecycle.h"
#include "headers/memory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DASHBOARD_MAX_AUDIT_EVENTS 256
#define DASHBOARD_MAX_LOG_ROWS     200

typedef struct
{
   char timestamp[32];
   char event[64];
   char detail[512];
   int rc;
   char phase[32];
   char hook[256];
} dashboard_audit_event_t;

typedef struct
{
   char timestamp[32];
   char source[32];
   char who[128];
   char what[128];
   char detail[512];
   char tag[64];
} dashboard_log_row_t;

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

static int dashboard_log_row_compare_desc(const void *lhs, const void *rhs)
{
   const dashboard_log_row_t *a = (const dashboard_log_row_t *)lhs;
   const dashboard_log_row_t *b = (const dashboard_log_row_t *)rhs;
   return strcmp(b->timestamp, a->timestamp);
}

char *api_memory_stats(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *resp = aimee_module_command_call(AIMEE_MEMORY_EVENT_COMMAND, AIMEE_MEMORY_STAGE_COMMAND,
                                           "stats_dashboard", req);
   cJSON_Delete(req);
   cJSON *data = resp ? cJSON_GetObjectItemCaseSensitive(resp, "dashboard") : NULL;
   char *json = data ? cJSON_PrintUnformatted(data) : NULL;
   cJSON_Delete(resp);
   return json;
}

char *api_logs(void)
{
   dashboard_log_row_t rows[DASHBOARD_MAX_LOG_ROWS];
   int row_count = 0;

   /* decision_log rows (DB2). */
   {
      db2_decision_log_row_t dl[100];
      int n = db2_decision_log_list(NULL, 100, dl, 100);
      for (int i = 0; i < n && row_count < DASHBOARD_MAX_LOG_ROWS; i++)
      {
         dashboard_log_row_t *row = &rows[row_count++];
         memset(row, 0, sizeof(*row));
         snprintf(row->timestamp, sizeof(row->timestamp), "%s", dl[i].created_at);
         snprintf(row->source, sizeof(row->source), "%s", "decision");
         snprintf(row->who, sizeof(row->who), "%s", dl[i].chosen);
         snprintf(row->what, sizeof(row->what), "%s", dl[i].options);
         snprintf(row->detail, sizeof(row->detail), "%s", dl[i].rationale);
         snprintf(row->tag, sizeof(row->tag), "%s", "decision");
      }
   }

   dashboard_audit_event_t audit_events[DASHBOARD_MAX_AUDIT_EVENTS];
   int audit_count = dashboard_load_audit_events(NULL, audit_events, DASHBOARD_MAX_AUDIT_EVENTS);
   for (int i = 0; i < audit_count && row_count < DASHBOARD_MAX_LOG_ROWS; i++)
   {
      dashboard_log_row_t *row = &rows[row_count++];
      memset(row, 0, sizeof(*row));
      snprintf(row->timestamp, sizeof(row->timestamp), "%s", audit_events[i].timestamp);
      snprintf(row->source, sizeof(row->source), "%s", "audit");
      snprintf(row->who, sizeof(row->who), "%s", audit_events[i].event);
      if (strcmp(audit_events[i].event, "plugin-hook") == 0)
      {
         snprintf(row->what, sizeof(row->what), "%s", audit_events[i].rc == 0 ? "ok" : "failure");
      }
      else
      {
         snprintf(row->what, sizeof(row->what), "%s", audit_events[i].event);
      }
      snprintf(row->detail, sizeof(row->detail), "%s", audit_events[i].detail);
      snprintf(row->tag, sizeof(row->tag), "%s", audit_events[i].event);
   }

   qsort(rows, (size_t)row_count, sizeof(rows[0]), dashboard_log_row_compare_desc);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < row_count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "source", rows[i].source);
      cJSON_AddStringToObject(obj, "who", rows[i].who);
      cJSON_AddStringToObject(obj, "what", rows[i].what);
      cJSON_AddStringToObject(obj, "detail", rows[i].detail);
      cJSON_AddStringToObject(obj, "timestamp", rows[i].timestamp);
      cJSON_AddStringToObject(obj, "tag", rows[i].tag);
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

char *api_dashboard_reminders(void)
{
   if (!db2_is_initialized())
      return NULL;
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return NULL;
   cJSON *response = aimee_module_command_call(
       AIMEE_MEMORY_EVENT_COMMAND, AIMEE_MEMORY_STAGE_COMMAND, "prospective_dashboard", args);
   cJSON_Delete(args);
   const cJSON *dashboard =
       response ? cJSON_GetObjectItemCaseSensitive(response, "dashboard") : NULL;
   char *result = dashboard ? cJSON_PrintUnformatted(dashboard) : NULL;
   cJSON_Delete(response);
   return result;
}

char *api_dashboard_recall(void)
{
   cJSON *args = cJSON_CreateObject(), *response = NULL;
   cJSON_AddStringToObject(args, "operation", "recall-dashboard");
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", args, &response);
   cJSON_Delete(args);
   if (rc <= 0 || !cJSON_IsObject(response))
   {
      cJSON_Delete(response);
      return NULL;
   }
   char *json = cJSON_PrintUnformatted(response);
   cJSON_Delete(response);
   return json;
}

char *api_dashboard_directives(void)
{
   if (!db2_is_initialized())
      return NULL;
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return NULL;
   cJSON *response = aimee_module_command_call(
       AIMEE_MEMORY_EVENT_COMMAND, AIMEE_MEMORY_STAGE_COMMAND, "directive_dashboard", args);
   cJSON_Delete(args);
   const cJSON *dashboard =
       response ? cJSON_GetObjectItemCaseSensitive(response, "dashboard") : NULL;
   char *result = dashboard ? cJSON_PrintUnformatted(dashboard) : NULL;
   cJSON_Delete(response);
   return result;
}
