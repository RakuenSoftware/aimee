#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

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
         if (copy && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "_compaction_boundary")))
            cJSON_AddBoolToObject(copy, "_compaction_boundary", 1);
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
      /* A legacy message array carries no original system/tools prefix proof. */
      for (int j = 0; j < ir.messages[i].n_blocks; j++)
      {
         free(ir.messages[i].blocks[j].thinking_binding);
         ir.messages[i].blocks[j].thinking_binding = NULL;
      }
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

cJSON *aimee_frontend_claude_transcript_import(const cJSON *records, const char *session_id)
{
   if (!session_id || !session_id[0] || !cJSON_IsArray(records) ||
       cJSON_GetArraySize(records) > 16384 || !aimee_ir_json_unique(records))
      return NULL;
   cJSON *native = cJSON_CreateArray(), *result = NULL;
   if (!native)
      return NULL;
   const cJSON *record;
   cJSON_ArrayForEach(record, records)
   {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "type"));
      const cJSON *session = cJSON_GetObjectItemCaseSensitive(record, "sessionId");
      const cJSON *sidechain = cJSON_GetObjectItemCaseSensitive(record, "isSidechain");
      if (!cJSON_IsObject(record) || !type ||
          (session && (!cJSON_IsString(session) || strcmp(session->valuestring, session_id))) ||
          (sidechain && (!cJSON_IsBool(sidechain) || cJSON_IsTrue(sidechain))))
         goto done;
      /* These known SDK bookkeeping records carry no conversation message.
       * A summary/compact-boundary/unknown record is deliberately unsupported. */
      if (!strcmp(type, "file-history-snapshot") || !strcmp(type, "progress") ||
          !strcmp(type, "queue-operation"))
         continue;
      if (!strcmp(type, "system"))
      {
         const char *subtype =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "subtype"));
         if (subtype && !strcmp(subtype, "turn_duration"))
            continue;
         goto done; /* Includes SDK compaction boundaries: never guess history. */
      }
      if ((strcmp(type, "user") && strcmp(type, "assistant")) || !cJSON_IsString(session))
         goto done;
      const cJSON *message = cJSON_GetObjectItemCaseSensitive(record, "message");
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "role"));
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
      if (!cJSON_IsObject(message) || !role || strcmp(role, type) ||
          (!cJSON_IsString(content) && !cJSON_IsArray(content)) ||
          cJSON_GetObjectItemCaseSensitive(message, "blocks"))
         goto done;
      /* Native message parsing, not caller-supplied canonical/context fields,
       * is the only admission path for this external source. */
      const cJSON *part;
      if (cJSON_IsArray(content))
         cJSON_ArrayForEach(
             part, content) if (cJSON_GetObjectItemCaseSensitive(part, "context")) goto done;
      cJSON *copy = cJSON_Duplicate(message, 1);
      if (!copy || !cJSON_AddItemToArray(native, copy))
      {
         cJSON_Delete(copy);
         goto done;
      }
      if (cJSON_GetArraySize(native) > 4096)
         goto done;
   }
   if (!cJSON_GetArraySize(native))
      goto done;
   cJSON *parsed = aimee_frontend_history_import(native, AIMEE_WIRE_ANTHROPIC);
   if (!parsed)
      goto done;
   result = cJSON_CreateArray();
   cJSON *item;
   cJSON_ArrayForEach(item, parsed)
   {
      aimee_message_t message = {0};
      if (aimee_ir_message_parse(item, &message))
      {
         cJSON_Delete(result);
         result = NULL;
         break;
      }
      for (int i = 0; i < message.n_blocks; ++i)
         aimee_ir_block_set_context(&message.blocks[i],
                                    !strcmp(message.role, "assistant") ? AIMEE_CTX_ORIGIN_MODEL
                                                                       : AIMEE_CTX_ORIGIN_USER,
                                    AIMEE_CTX_AUTH_EVIDENCE, AIMEE_CTX_TRUST_UNVERIFIED,
                                    AIMEE_CTX_SENS_INTERNAL, 1, "external-sdk", "claude-code", 0);
      cJSON *observation = aimee_ir_message_json(&message);
      aimee_message_free(&message);
      if (!observation || !cJSON_AddItemToArray(result, observation))
      {
         cJSON_Delete(observation);
         cJSON_Delete(result);
         result = NULL;
         break;
      }
   }
   cJSON_Delete(parsed);
done:
   cJSON_Delete(native);
   return result;
}
