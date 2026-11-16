#ifndef AIMEE_ANTHROPIC_REFUSAL_H
#define AIMEE_ANTHROPIC_REFUSAL_H

#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include <string.h>

/* Transport hints are diagnostic data, not permission or retry instructions.
 * Borrow only the typed fields; unknown provider extras never enter the IR. */
static inline int anthropic_refusal_read(const cJSON *value, const char *reason,
                                         aimee_refusal_details_t *details, int *present)
{
   *details = (aimee_refusal_details_t){0};
   *present = 0;
   if (!value || cJSON_IsNull(value))
      return 0;
   if (!cJSON_IsObject(value) || !aimee_ir_json_unique(value) || !reason ||
       strcmp(reason, "refusal"))
      return -1;
   const cJSON *type = cJSON_GetObjectItemCaseSensitive(value, "type");
   const cJSON *category = cJSON_GetObjectItemCaseSensitive(value, "category");
   const cJSON *explanation = cJSON_GetObjectItemCaseSensitive(value, "explanation");
   if (!cJSON_IsString(type) || strcmp(type->valuestring, "refusal") ||
       (category && !cJSON_IsNull(category) && !cJSON_IsString(category)) ||
       (explanation && !cJSON_IsNull(explanation) && !cJSON_IsString(explanation)))
      return -1;
   details->category = cJSON_IsString(category) ? category->valuestring : NULL;
   details->explanation = cJSON_IsString(explanation) ? explanation->valuestring : NULL;
   if (!aimee_refusal_details_valid(details))
      return -1;
   *present = 1;
   return 0;
}

static inline cJSON *anthropic_refusal_json(const aimee_refusal_details_t *details)
{
   if (!details || !aimee_refusal_details_valid(details))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   if (!out || !cJSON_AddStringToObject(out, "type", "refusal") ||
       !(details->category ? cJSON_AddStringToObject(out, "category", details->category)
                           : cJSON_AddNullToObject(out, "category")) ||
       !(details->explanation ? cJSON_AddStringToObject(out, "explanation", details->explanation)
                              : cJSON_AddNullToObject(out, "explanation")))
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}
#endif
