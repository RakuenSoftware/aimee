#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"

/* Migration is an entry adapter, never a fallback inside the execution loop. */
cJSON *aimee_frontend_history_import(const cJSON *messages, aimee_wire_t legacy_wire)
{
   if (legacy_wire != AIMEE_WIRE_ANTHROPIC && legacy_wire != AIMEE_WIRE_OPENAI_CHAT &&
       legacy_wire != AIMEE_WIRE_RESPONSES)
      return NULL;
   if (!messages)
      return cJSON_CreateArray();
   if (!cJSON_IsArray(messages))
      return NULL;
   cJSON *out = cJSON_CreateArray();
   if (!out)
      return NULL;
   int canonical = 0, native = 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, messages)
   {
      if (cJSON_GetObjectItemCaseSensitive(item, "blocks"))
         canonical++;
      else
         native++;
   }
   if (canonical && native)
      goto fail;
   if (!native)
   {
      cJSON_ArrayForEach(item, messages)
      {
         aimee_message_t message = {0};
         if (aimee_ir_message_parse(item, &message) != 0)
            goto fail;
         cJSON *copy = aimee_ir_message_json(&message);
         aimee_message_free(&message);
         if (!copy || !cJSON_AddItemToArray(out, copy))
         {
            cJSON_Delete(copy);
            goto fail;
         }
      }
      return out;
   }
   cJSON *request = cJSON_CreateObject();
   if (!request)
      goto fail;
   cJSON_AddStringToObject(request, "model", "history-migration");
   cJSON_AddNumberToObject(request, "max_tokens", 1);
   cJSON_AddItemReferenceToObject(
       request, legacy_wire == AIMEE_WIRE_RESPONSES ? "input" : "messages", (cJSON *)messages);
   aimee_request_t ir = {0};
   char error[128];
   int rc = legacy_wire == AIMEE_WIRE_ANTHROPIC
                ? anthropic_frontend_parse(request, &ir, error, sizeof error)
            : legacy_wire == AIMEE_WIRE_RESPONSES
                ? responses_frontend_parse(request, &ir, error, sizeof error)
                : openai_frontend_parse(request, &ir, error, sizeof error);
   cJSON_Delete(request);
   if (rc != 0)
   {
      aimee_request_free(&ir);
      goto fail;
   }
   for (int i = 0; i < ir.n_messages; i++)
   {
      cJSON *copy = aimee_ir_message_json(&ir.messages[i]);
      aimee_message_t validated = {0};
      int valid = copy && aimee_ir_message_parse(copy, &validated) == 0;
      aimee_message_free(&validated);
      if (!valid || !cJSON_AddItemToArray(out, copy))
      {
         cJSON_Delete(copy);
         aimee_request_free(&ir);
         goto fail;
      }
   }
   aimee_request_free(&ir);
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}
