/* render.c: JSON output formatting, struct-to-JSON converters, field filtering */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keys stripped in compact profile */
static const char *compact_strip[] = {"description", "sensitivity", "created_at", "updated_at",
                                      "scanned_at",  "domain",      NULL};

static int should_strip(const char *key)
{
   for (int i = 0; compact_strip[i]; i++)
   {
      if (strcmp(key, compact_strip[i]) == 0)
         return 1;
   }
   return 0;
}

static void strip_compact_keys(cJSON *json)
{
   if (!cJSON_IsObject(json))
   {
      if (cJSON_IsArray(json))
      {
         cJSON *el;
         cJSON_ArrayForEach(el, json)
         {
            strip_compact_keys(el);
         }
      }
      return;
   }

   /* Collect keys to delete */
   const char *to_delete[16];
   int del_count = 0;

   cJSON *child = json->child;
   while (child)
   {
      if (child->string && should_strip(child->string) && del_count < 16)
      {
         to_delete[del_count++] = child->string;
      }
      if (cJSON_IsObject(child) || cJSON_IsArray(child))
         strip_compact_keys(child);
      child = child->next;
   }

   for (int i = 0; i < del_count; i++)
      cJSON_DeleteItemFromObjectCaseSensitive(json, to_delete[i]);
}

static void filter_fields(cJSON *json, const char *fields)
{
   if (!cJSON_IsObject(json) || !fields)
      return;

   /* Parse comma-separated field list */
   char buf[1024];
   snprintf(buf, sizeof(buf), "%s", fields);
   char *keep[64];
   int keep_count = 0;

   char *save;
   char *tok = strtok_r(buf, ",", &save);
   while (tok && keep_count < 64)
   {
      while (*tok == ' ')
         tok++;
      char *end = tok + strlen(tok) - 1;
      while (end > tok && *end == ' ')
         *end-- = '\0';
      if (*tok)
         keep[keep_count++] = tok;
      tok = strtok_r(NULL, ",", &save);
   }

   /* Collect keys to delete */
   const char *to_delete[128];
   int del_count = 0;

   cJSON *child = json->child;
   while (child)
   {
      if (child->string)
      {
         int found = 0;
         for (int i = 0; i < keep_count; i++)
         {
            if (strcmp(child->string, keep[i]) == 0)
            {
               found = 1;
               break;
            }
         }
         /* Always keep "status" */
         if (strcmp(child->string, "status") == 0)
            found = 1;
         if (!found && del_count < 128)
            to_delete[del_count++] = child->string;
      }
      child = child->next;
   }

   for (int i = 0; i < del_count; i++)
      cJSON_DeleteItemFromObjectCaseSensitive(json, to_delete[i]);
}

/* --- emit_json_ctx --- */

void emit_json_ctx(cJSON *json, const char *fields, const char *profile)
{
   if (!json)
      return;

   /* Apply compact profile */
   if (profile && strcmp(profile, "compact") == 0)
      strip_compact_keys(json);

   /* Apply field filter */
   if (fields)
   {
      if (cJSON_IsObject(json))
      {
         filter_fields(json, fields);
      }
      else if (cJSON_IsArray(json))
      {
         cJSON *el;
         cJSON_ArrayForEach(el, json)
         {
            filter_fields(el, fields);
         }
      }
   }

   char *str = cJSON_PrintUnformatted(json);
   if (str)
   {
      puts(str);
      free(str);
   }
   cJSON_Delete(json);
}

/* --- emit_ok_ctx / emit_ok_kv_ctx --- */

void emit_ok_ctx(const char *fields, const char *profile)
{
   emit_json_ctx(jo_ok(), fields, profile);
}

void emit_ok_kv_ctx(const char *key, const char *value, const char *fields, const char *profile)
{
   cJSON *j = jo_ok();
   if (key && value)
      cJSON_AddStringToObject(j, key, value);
   emit_json_ctx(j, fields, profile);
}

/* --- Struct-to-JSON converters --- */

cJSON *memory_to_json(const memory_t *m)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddNumberToObject(j, "id", (double)m->id);
   cJSON_AddStringToObject(j, "tier", m->tier);
   cJSON_AddStringToObject(j, "kind", m->kind);
   cJSON_AddStringToObject(j, "key", m->key);
   cJSON_AddStringToObject(j, "content", m->content);
   cJSON_AddNumberToObject(j, "confidence", m->confidence);
   cJSON_AddNumberToObject(j, "use_count", m->use_count);
   cJSON_AddStringToObject(j, "last_used_at", m->last_used_at);
   cJSON_AddStringToObject(j, "created_at", m->created_at);
   cJSON_AddStringToObject(j, "updated_at", m->updated_at);
   cJSON_AddStringToObject(j, "source_session", m->source_session);
   cJSON_AddStringToObject(j, "provenance_category", m->provenance_category);
   return j;
}

cJSON *search_result_to_json(const search_result_t *r)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddStringToObject(j, "session_id", r->session_id);
   cJSON_AddNumberToObject(j, "seq", r->seq);
   cJSON_AddStringToObject(j, "file_path", r->file_path);
   cJSON_AddNumberToObject(j, "start_line", r->start_line);
   cJSON_AddNumberToObject(j, "end_line", r->end_line);
   cJSON_AddStringToObject(j, "summary", r->summary);
   cJSON_AddNumberToObject(j, "score", r->score);

   cJSON *files = cJSON_AddArrayToObject(j, "files");
   for (int i = 0; i < r->file_count; i++)
      cJSON_AddItemToArray(files, cJSON_CreateString(r->files[i]));

   return j;
}

cJSON *aimee_task_to_json(const aimee_task_t *t)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddNumberToObject(j, "id", (double)t->id);
   cJSON_AddNumberToObject(j, "parent_id", (double)t->parent_id);
   cJSON_AddStringToObject(j, "title", t->title);
   cJSON_AddStringToObject(j, "state", t->state);
   cJSON_AddNumberToObject(j, "confidence", t->confidence);
   cJSON_AddStringToObject(j, "created_at", t->created_at);
   cJSON_AddStringToObject(j, "updated_at", t->updated_at);
   cJSON_AddStringToObject(j, "session_id", t->session_id);
   return j;
}

cJSON *decision_to_json(const kb_store_decision_log_row_t *d)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddNumberToObject(j, "id", (double)d->id);
   cJSON_AddNumberToObject(j, "task_id", (double)d->task_id);
   cJSON_AddStringToObject(j, "options", d->options);
   cJSON_AddStringToObject(j, "chosen", d->chosen);
   cJSON_AddStringToObject(j, "rationale", d->rationale);
   cJSON_AddStringToObject(j, "assumptions", d->assumptions);
   cJSON_AddStringToObject(j, "outcome", d->outcome);
   cJSON_AddStringToObject(j, "created_at", d->created_at);
   return j;
}

cJSON *checkpoint_to_json(const db1_checkpoint_t *c)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddNumberToObject(j, "id", (double)c->id);
   cJSON_AddNumberToObject(j, "task_id", (double)c->task_id);
   cJSON_AddStringToObject(j, "session_id", c->session_id);
   cJSON_AddStringToObject(j, "label", c->label);
   cJSON_AddStringToObject(j, "snapshot", c->snapshot);
   cJSON_AddStringToObject(j, "created_at", c->created_at);
   return j;
}

cJSON *anti_pattern_to_json(const anti_pattern_t *a)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddNumberToObject(j, "id", (double)a->id);
   cJSON_AddStringToObject(j, "pattern", a->pattern);
   cJSON_AddStringToObject(j, "description", a->description);
   cJSON_AddStringToObject(j, "source", a->source);
   cJSON_AddStringToObject(j, "source_ref", a->source_ref);
   cJSON_AddNumberToObject(j, "hit_count", a->hit_count);
   cJSON_AddNumberToObject(j, "confidence", a->confidence);
   return j;
}

cJSON *conflict_to_json(const conflict_t *c)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddNumberToObject(j, "id", (double)c->id);
   cJSON_AddNumberToObject(j, "memory_a", (double)c->memory_a);
   cJSON_AddNumberToObject(j, "memory_b", (double)c->memory_b);
   cJSON_AddStringToObject(j, "detected_at", c->detected_at);
   cJSON_AddBoolToObject(j, "resolved", c->resolved);
   cJSON_AddStringToObject(j, "resolution", c->resolution);
   return j;
}

/* --- agent_result_to_json --- */

cJSON *agent_result_to_json(const agent_result_t *result)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "status", result->success ? "success" : "error");
   cJSON_AddStringToObject(obj, "response", result->response ? result->response : "");
   cJSON_AddNumberToObject(obj, "turns", result->turns);
   cJSON_AddNumberToObject(obj, "tool_calls", result->tool_calls);
   cJSON_AddNumberToObject(obj, "rescue_recoveries", result->rescue_recoveries);
   cJSON_AddNumberToObject(obj, "prompt_tokens", result->prompt_tokens);
   cJSON_AddNumberToObject(obj, "completion_tokens", result->completion_tokens);
   cJSON_AddNumberToObject(obj, "latency_ms", result->latency_ms);
   if (result->confidence >= 0)
      cJSON_AddNumberToObject(obj, "confidence", result->confidence);
   if (result->error[0])
      cJSON_AddStringToObject(obj, "error", result->error);
   return obj;
}
