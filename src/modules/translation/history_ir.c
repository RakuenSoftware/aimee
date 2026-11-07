#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include "aimee_sha256.h"
#include <stdlib.h>
#include <string.h>

/* Migration is an entry adapter, never a fallback inside the execution loop. */
static cJSON *history_import(const cJSON *messages, aimee_wire_t legacy_wire, int *removed)
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
      cJSON_Delete(out);
      return aimee_ir_history_clone(messages);
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
         if (removed && ir.messages[i].blocks[j].thinking_binding)
            (*removed)++;
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

cJSON *aimee_frontend_history_import(const cJSON *messages, aimee_wire_t wire)
{
   return history_import(messages, wire, NULL);
}

/* Typed defaults may be added, but an exact import retains every source field. */
static int history_preserves_source(const cJSON *source, const cJSON *target)
{
   if (cJSON_IsObject(source))
   {
      if (!cJSON_IsObject(target))
         return 0;
      const cJSON *field;
      cJSON_ArrayForEach(field, source) if (!history_preserves_source(
                                                field, cJSON_GetObjectItemCaseSensitive(
                                                           target, field->string))) return 0;
      return 1;
   }
   if (cJSON_IsArray(source))
   {
      if (!cJSON_IsArray(target) || cJSON_GetArraySize(source) != cJSON_GetArraySize(target))
         return 0;
      const cJSON *left = source->child, *right = target->child;
      while (left && right)
      {
         if (!history_preserves_source(left, right))
            return 0;
         left = left->next;
         right = right->next;
      }
      return 1;
   }
   return target && cJSON_Compare(source, target, 1);
}

/* Migration precedes compaction, so it must not impose the model request's
 * message-count cap on an otherwise valid, frame-bounded legacy transcript. */
static int history_import_cycles_admit(const cJSON *messages)
{
   const cJSON *item;
   cJSON_ArrayForEach(item, messages)
   {
      aimee_message_t message = {0};
      if (aimee_ir_message_parse(item, &message))
         return -1;
      int reserved = 0;
      for (int i = 0; i < message.n_blocks; i++)
      {
         const char *domain = message.blocks[i].context.revision_domain;
         if (domain &&
             (!strcmp(domain, "native-interruption") || !strcmp(domain, "history-repair")))
            reserved = 1;
      }
      aimee_message_free(&message);
      if (reserved)
         return -1;
   }
   cJSON *copy = cJSON_Duplicate(messages, 1);
   int repairs = copy ? aimee_ir_history_repair(copy) : -1;
   cJSON_Delete(copy);
   return repairs == 0 ? 0 : -1;
}

/* Report source bytes exactly as stored; metadata is never a model message. */
cJSON *aimee_frontend_history_import_report(const char *raw, const char *provider,
                                            const char *source_session, const char *source_agent,
                                            cJSON **out_report)
{
   if (!out_report)
      return NULL;
   *out_report = NULL;
   if (!raw || !provider || !source_session || !source_agent)
      return NULL;
   char digest[65];
   if (aimee_sha256_hex(raw, strlen(raw), digest))
      return NULL;
   cJSON *source = aimee_ir_json_parse_frame(raw, strlen(raw));
   int canonical = cJSON_IsArray(source),
       count = cJSON_IsArray(source) ? cJSON_GetArraySize(source) : 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, source) if (!cJSON_GetObjectItemCaseSensitive(item, "blocks"))
       canonical = 0;
   int screen = !strcmp(provider, "tmux") || !strcmp(provider, "tmux-cli");
   aimee_wire_t wire = !strcmp(provider, "anthropic") ? AIMEE_WIRE_ANTHROPIC
                       : !strcmp(provider, "chatgpt") ? AIMEE_WIRE_RESPONSES
                                                      : AIMEE_WIRE_OPENAI_CHAT;
   const char *format = screen                         ? "tmux_text"
                        : canonical                    ? "canonical"
                        : !cJSON_IsArray(source)       ? "unknown"
                        : wire == AIMEE_WIRE_ANTHROPIC ? "anthropic"
                        : wire == AIMEE_WIRE_RESPONSES ? "responses"
                                                       : "openai_chat";
   int removed = 0, chunks = 0;
   cJSON *history =
       !screen && cJSON_IsArray(source) ? history_import(source, wire, &removed) : NULL;
   if (history && history_import_cycles_admit(history))
   {
      cJSON_Delete(history);
      history = NULL;
   }
   const char *outcome =
       history  ? canonical && history_preserves_source(source, history) ? "exact" : "normalized"
       : screen ? "archived"
                : "refused";
   if (screen)
   {
      size_t length = strlen(raw), at = 0;
      while (at < length)
      {
         size_t n = length - at > 65536 ? 65536 : length - at;
         if (at + n < length)
            while (n && (((unsigned char)raw[at + n] & 0xc0) == 0x80))
               n--;
         if (!n)
            break;
         chunks++;
         at += n;
      }
      if (!chunks || chunks > 4096)
         outcome = "refused";
   }
   cJSON *report = cJSON_CreateObject();
   if (!report || !cJSON_AddNumberToObject(report, "version", 1) ||
       !cJSON_AddStringToObject(report, "source_format", format) ||
       !cJSON_AddStringToObject(report, "outcome", outcome) ||
       !cJSON_AddStringToObject(report, "source_sha256", digest) ||
       !cJSON_AddStringToObject(report, "source_session", source_session) ||
       !cJSON_AddStringToObject(report, "source_agent", source_agent) ||
       !cJSON_AddStringToObject(report, "source_provider", provider) ||
       !cJSON_AddNumberToObject(report, "source_messages", screen ? 0 : count) ||
       !cJSON_AddNumberToObject(report, "imported_messages",
                                history ? cJSON_GetArraySize(history) : 0) ||
       !cJSON_AddNumberToObject(report, "removed_thinking_bindings", removed) ||
       !cJSON_AddNumberToObject(report, "archive_chunks",
                                !strcmp(outcome, "archived") ? chunks : 0) ||
       !cJSON_AddStringToObject(report, "tool_structure", history ? "preserved" : "unrecoverable"))
   {
      cJSON_Delete(report);
      cJSON_Delete(history);
      history = NULL;
   }
   else
      *out_report = report;
   cJSON_Delete(source);
   return history;
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
