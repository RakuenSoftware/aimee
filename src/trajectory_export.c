/* trajectory_export.c: DB1 interaction events -> replayable trajectory JSON. */
#include "audit_ledger.h"
#include "trajectory.h"

#include "cJSON.h"
#include "interaction_events.h"
#include "memory_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRAJ_MAX_EVENTS          2048
#define TRAJ_DEFAULT_RESULT_CAP  512
#define TRAJ_REDACTION_BUF_BYTES 8192

static const char *json_string_field(cJSON *obj, const char *a, const char *b, const char *c)
{
   if (!cJSON_IsObject(obj))
      return NULL;
   const char *names[3] = {a, b, c};
   for (int i = 0; i < 3; i++)
   {
      if (!names[i])
         continue;
      cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, names[i]);
      if (cJSON_IsString(v) && v->valuestring)
         return v->valuestring;
   }
   return NULL;
}

static cJSON *payload_parse_or_wrap(const char *payload)
{
   cJSON *parsed = cJSON_Parse(payload && payload[0] ? payload : "{}");
   if (parsed)
      return parsed;
   cJSON *wrap = cJSON_CreateObject();
   cJSON_AddStringToObject(wrap, "raw", payload ? payload : "");
   return wrap;
}

static char *payload_content(cJSON *payload)
{
   const char *s = json_string_field(payload, "content", "text", "message");
   if (s)
      return strdup(s);
   s = json_string_field(payload, "prompt", "result", "output");
   if (s)
      return strdup(s);
   return cJSON_PrintUnformatted(payload);
}

static uint64_t fnv1a64(const char *s)
{
   uint64_t h = 1469598103934665603ULL;
   const unsigned char *p = (const unsigned char *)(s ? s : "");
   while (*p)
   {
      h ^= (uint64_t)*p++;
      h *= 1099511628211ULL;
   }
   return h;
}

static void copy_json_member(cJSON *dst, const char *dst_name, cJSON *src, const char *src_name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(src, src_name);
   if (v)
      cJSON_AddItemToObject(dst, dst_name, cJSON_Duplicate(v, 1));
}

static void add_turn_step(cJSON *steps, const char *role, const char *content)
{
   cJSON *step = cJSON_CreateObject();
   cJSON_AddStringToObject(step, "role", role);
   cJSON_AddStringToObject(step, "content", content ? content : "");
   cJSON_AddItemToArray(steps, step);
}

static void add_tool_call_step(cJSON *steps, cJSON *payload)
{
   const char *name = json_string_field(payload, "name", "tool", "tool_name");
   cJSON *step = cJSON_CreateObject();
   cJSON *calls = cJSON_AddArrayToObject(step, "tool_calls");
   cJSON *call = cJSON_CreateObject();
   cJSON_AddStringToObject(step, "role", "assistant");
   cJSON_AddStringToObject(step, "content", "");
   cJSON_AddStringToObject(call, "name", name ? name : "unknown");

   cJSON *args = cJSON_GetObjectItemCaseSensitive(payload, "args");
   if (!args)
      args = cJSON_GetObjectItemCaseSensitive(payload, "input");
   if (args)
      cJSON_AddItemToObject(call, "args", cJSON_Duplicate(args, 1));
   else
      cJSON_AddItemToObject(call, "args", cJSON_CreateObject());

   cJSON_AddItemToArray(calls, call);
   cJSON_AddItemToArray(steps, step);
}

static void add_tool_result_step(cJSON *steps, cJSON *payload)
{
   const char *name = json_string_field(payload, "name", "tool", "tool_name");
   char *result = payload_content(payload);
   cJSON *step = cJSON_CreateObject();
   cJSON_AddStringToObject(step, "role", "tool");
   cJSON_AddStringToObject(step, "name", name ? name : "unknown");
   cJSON_AddStringToObject(step, "result", result ? result : "");
   cJSON_AddNullToObject(step, "result_ref");
   free(result);
   cJSON_AddItemToArray(steps, step);
}

static int event_is_failure(const ie_event_row_t *row)
{
   return row && row->outcome[0] && strcmp(row->outcome, "ok") != 0 &&
          strcmp(row->outcome, "success") != 0;
}

static void add_event_to_trajectory(cJSON *root, cJSON *steps, const ie_event_row_t *row)
{
   cJSON *payload = payload_parse_or_wrap(row->payload);
   if (!payload)
      return;

   if (strcmp(row->event_type, "user_turn") == 0 || strcmp(row->event_type, "user_correction") == 0)
   {
      char *content = payload_content(payload);
      add_turn_step(steps, "user", content);
      free(content);
   }
   else if (strcmp(row->event_type, "agent_turn") == 0)
   {
      char *content = payload_content(payload);
      cJSON *step = cJSON_CreateObject();
      cJSON_AddStringToObject(step, "role", "assistant");
      cJSON_AddStringToObject(step, "content", content ? content : "");
      copy_json_member(step, "tool_calls", payload, "tool_calls");
      cJSON_AddItemToArray(steps, step);
      free(content);
   }
   else if (strcmp(row->event_type, "tool_call") == 0)
      add_tool_call_step(steps, payload);
   else if (strcmp(row->event_type, "tool_outcome") == 0)
      add_tool_result_step(steps, payload);

   cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
   const char *system_text = json_string_field(payload, "system", "system_prompt", NULL);
   if (system_text && cJSON_IsString(system) && (!system->valuestring || !system->valuestring[0]))
      cJSON_SetValuestring(system, system_text);

   cJSON_Delete(payload);
}

static void compress_tool_steps(cJSON *steps, int cap)
{
   if (!cJSON_IsArray(steps))
      return;
   if (cap <= 0)
      cap = TRAJ_DEFAULT_RESULT_CAP;

   cJSON *step = NULL;
   cJSON_ArrayForEach(step, steps)
   {
      cJSON *role = cJSON_GetObjectItemCaseSensitive(step, "role");
      cJSON *result = cJSON_GetObjectItemCaseSensitive(step, "result");
      if (!cJSON_IsString(role) || strcmp(role->valuestring, "tool") != 0 ||
          !cJSON_IsString(result) || !result->valuestring)
         continue;
      int len = (int)strlen(result->valuestring);
      if (len <= cap)
         continue;
      cJSON *name = cJSON_GetObjectItemCaseSensitive(step, "name");
      char ref[256];
      snprintf(ref, sizeof(ref), "%s bytes=%d hash=fnv1a64:%016llx",
               cJSON_IsString(name) ? name->valuestring : "tool", len,
               (unsigned long long)fnv1a64(result->valuestring));
      cJSON_SetValuestring(result, "[compressed tool result]");
      cJSON_ReplaceItemInObject(step, "result_ref", cJSON_CreateString(ref));
   }
}

static int redact_json_strings(cJSON *node, int redaction_buf_bytes)
{
   if (!node)
      return 0;
   if (cJSON_IsString(node) && node->valuestring)
   {
      int cap = redaction_buf_bytes > 0 ? redaction_buf_bytes : TRAJ_REDACTION_BUF_BYTES;
      char *redacted = malloc((size_t)cap);
      if (!redacted)
         return -1;
      int rc = gate_check_sensitive(node->valuestring, redacted, (size_t)cap);
      if (rc == 2)
      {
         free(redacted);
         return -1;
      }
      if (rc == 1 && cJSON_SetValuestring(node, redacted) == NULL)
      {
         free(redacted);
         return -1;
      }
      free(redacted);
      return 0;
   }
   cJSON *child = node->child;
   while (child)
   {
      if (redact_json_strings(child, redaction_buf_bytes) != 0)
         return -1;
      child = child->next;
   }
   return 0;
}

int trajectory_export(const char *selector, const trajectory_opts_t *opts, char **out_json)
{
   if (!selector || !selector[0] || !out_json)
      return -1;
   *out_json = NULL;

   ie_event_row_t *rows = calloc(TRAJ_MAX_EVENTS, sizeof(*rows));
   if (!rows)
      return -1;
   int n = ie_list_for_session(selector, rows, TRAJ_MAX_EVENTS);
   if (n <= 0)
   {
      free(rows);
      return -1;
   }

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "schema", "aimee.trajectory.v1");
   cJSON_AddStringToObject(root, "system", "");
   cJSON *steps = cJSON_AddArrayToObject(root, "steps");

   int failed = 0;
   int corrected = 0;
   for (int i = 0; i < n; i++)
   {
      add_event_to_trajectory(root, steps, &rows[i]);
      if (event_is_failure(&rows[i]))
         failed = 1;
      if (strcmp(rows[i].event_type, "user_correction") == 0)
         corrected = 1;
   }

   if (opts && opts->compress)
      compress_tool_steps(steps, opts->max_tool_result_bytes);

   cJSON *outcome = cJSON_AddObjectToObject(root, "outcome");
   cJSON_AddStringToObject(outcome, "status", (failed || corrected) ? "failure" : "success");
   cJSON_AddNumberToObject(outcome, "reward", (failed || corrected) ? 0.0 : 1.0);
   cJSON_AddStringToObject(outcome, "signal", corrected ? "user_correction" : "interaction_events");

   cJSON *meta = cJSON_AddObjectToObject(root, "meta");
   cJSON_AddStringToObject(meta, "selector", selector);
   cJSON_AddNumberToObject(meta, "event_count", n);
   cJSON_AddStringToObject(meta, "started_at", rows[0].created_at);
   cJSON_AddStringToObject(meta, "ended_at", rows[n - 1].created_at);

   /* Attach governed-action audit rows (S3) that fall within this trajectory's
    * time window. audit.log ts and interaction_events.created_at share the same
    * ISO-8601 UTC format, so the window bound is a lexicographic compare. NOTE:
    * the audit ledger is not session-keyed, so a window that overlaps concurrent
    * sessions may include their actions — a time-scoped overlay, not a strict
    * per-session filter. Empty/absent when the writer (S7) is disabled. */
   cJSON *governed = audit_ledger_read(rows[0].created_at, rows[n - 1].created_at);
   if (governed)
      cJSON_AddItemToObject(root, "governed_actions", governed);

   if (!opts || opts->redact)
   {
      if (redact_json_strings(root, opts ? opts->redaction_buf_bytes : 0) != 0)
      {
         cJSON_Delete(root);
         free(rows);
         return -1;
      }
   }

   *out_json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   free(rows);
   return *out_json ? 0 : -1;
}
