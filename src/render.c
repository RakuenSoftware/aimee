/* render.c: JSON output formatting, struct-to-JSON converters, field filtering */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- emit_json_ctx --- */

/* --- emit_ok_ctx / emit_ok_kv_ctx --- */

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

/* --- agent_result_to_json --- */
