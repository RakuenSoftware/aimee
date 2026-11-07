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
