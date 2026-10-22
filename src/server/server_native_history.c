#include "server_native_history.h"
#include "db1_client/primary_sessions.h"
#include <aimee/ir/session_event.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char warning[] =
    "The host lost this tool's completion result. Its effects are unknown; interruption does not "
    "prove that it failed or was cancelled. Do not repeat an effect without explicit user "
    "authorization.";
static const char *text(const cJSON *value, const char *name)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, name));
}
static int equal(const char *a, const char *b)
{
   return a && b && !strcmp(a, b);
}
static int exact(const cJSON *value, int fields)
{
   return cJSON_IsObject(value) && aimee_ir_json_unique(value) &&
          cJSON_GetArraySize(value) == fields;
}
static int bounded(const cJSON *value, const char *name, size_t maximum)
{
   const char *s = text(value, name);
   return s && s[0] && strlen(s) <= maximum;
}
static int hex(const cJSON *value, const char *name, size_t size)
{
   const char *s = text(value, name);
   return s && strlen(s) == size && strspn(s, "0123456789abcdef") == size;
}
static int positive(const cJSON *value, const char *name)
{
   const char *s = text(value, name);
   if (!s || !s[0] || s[0] == '0' || strlen(s) > 19 || strspn(s, "0123456789") != strlen(s))
      return 0;
   return strlen(s) < 19 || strcmp(s, "9223372036854775807") <= 0;
}
/* Operator cancellation is a different lifecycle authority from native
 * containment. The Go-owned loader rechecks its actual durable terminal. */
static int operator_owner(const cJSON *entry, const cJSON *owner, const cJSON *source)
{
   if (!exact(owner, 1) || !bounded(source, "turn_id", 128) || !positive(source, "sequence"))
      return 0;
   const cJSON *index = cJSON_GetObjectItemCaseSensitive(source, "block_index");
   if (!cJSON_IsNumber(index) || index->valuedouble < 0 || index->valuedouble >= 4096 ||
       index->valuedouble != index->valueint)
      return 0;
   char marker[160], receipt[224];
   snprintf(marker, sizeof marker, "operator-cancel:%s", text(source, "turn_id"));
   snprintf(receipt, sizeof receipt, "operator-interruption:%s:%s:%d", text(source, "turn_id"),
            text(source, "sequence"), index->valueint);
   return equal(text(owner, "operator_cancel_event_id"), marker) &&
          equal(text(entry, "event_id"), receipt);
}
static int valid_entry(const cJSON *entry)
{
   const cJSON *r = cJSON_GetObjectItemCaseSensitive(entry, "receipt");
   const cJSON *call = cJSON_GetObjectItemCaseSensitive(entry, "call");
   const cJSON *source = cJSON_GetObjectItemCaseSensitive(r, "source"),
               *owner = cJSON_GetObjectItemCaseSensitive(r, "owner");
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(r, "version"),
               *index = cJSON_GetObjectItemCaseSensitive(source, "block_index");
   int operator= operator_owner(entry, owner, source);
   int native = exact(owner, 4) && hex(owner, "owner_id", 32) && hex(owner, "binding_sha256", 64) &&
                hex(owner, "containment_sha256", 64) &&
                (equal(text(owner, "epoch"), "0") || hex(owner, "epoch", 32));
   return exact(entry, 3) && bounded(entry, "event_id", 1024) && cJSON_IsObject(call) &&
          aimee_ir_json_unique(call) && equal(text(call, "type"), "tool_use") &&
          equal(text(call, "tool_id"), text(source, "tool_id")) &&
          equal(text(call, "name"), text(source, "name")) &&
          cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(call, "input")) && exact(r, 6) &&
          cJSON_IsNumber(version) && version->valuedouble == 1 && exact(source, 9) &&
          bounded(source, "session_id", 128) && bounded(source, "turn_id", 128) &&
          bounded(source, "event_id", 1024) && bounded(source, "actor", operator? 159 : 128) &&
          bounded(source, "message_id", 1024) && bounded(source, "tool_id", 1024) &&
          bounded(source, "name", 1024) && positive(source, "sequence") && cJSON_IsNumber(index) &&
          index->valuedouble >= 0 && index->valuedouble < 4096 &&
          index->valuedouble == index->valueint && (operator|| native) &&
          positive(r, "terminal_sequence") &&
          (!operator|| strtoull(text(source, "sequence"), NULL, 10) <
           strtoull(text(r, "terminal_sequence"), NULL, 10)) &&
          equal(text(r, "outcome"), "unknown") &&
          equal(text(r, "retry"), "explicit_permission_required");
}
static int same_call(const cJSON *left, const cJSON *right)
{
   if (!equal(text(left, "type"), "tool_use") || !equal(text(right, "type"), "tool_use") ||
       !equal(text(left, "tool_id"), text(right, "tool_id")) ||
       !equal(text(left, "name"), text(right, "name")))
      return 0;
   const char *a = text(left, "namespace"), *b = text(right, "namespace");
   if (strcmp(a ? a : "", b ? b : ""))
      return 0;
   cJSON *ac = aimee_ir_json_canonical_copy(cJSON_GetObjectItemCaseSensitive(left, "input"));
   cJSON *bc = aimee_ir_json_canonical_copy(cJSON_GetObjectItemCaseSensitive(right, "input"));
   char *aw = ac ? cJSON_PrintUnformatted(ac) : NULL, *bw = bc ? cJSON_PrintUnformatted(bc) : NULL;
   int same = aw && bw && !strcmp(aw, bw);
   free(aw);
   free(bw);
   cJSON_Delete(ac);
   cJSON_Delete(bc);
   return same;
}
int server_native_history_requires_permission(const cJSON *receipts)
{
   return cJSON_IsArray(receipts) ? cJSON_GetArraySize(receipts) > 0 : -1;
}
int server_native_history_admit(const cJSON *messages, const cJSON *receipts)
{
   if (!cJSON_IsArray(messages) || !cJSON_IsArray(receipts) || cJSON_GetArraySize(receipts) > 4096)
      return -1;
   const cJSON *entry, *other;
   cJSON_ArrayForEach(entry, receipts)
   {
      if (!valid_entry(entry))
         return -1;
      for (other = entry->next; other; other = other->next)
         if (equal(text(entry, "event_id"), text(other, "event_id")))
            return -1;
   }
   cJSON *copy = cJSON_Duplicate(messages, 1);
   if (!copy)
      return -1;
   unsigned char *seen = calloc((size_t)cJSON_GetArraySize(receipts) + 1, 1);
   if (!seen)
   {
      cJSON_Delete(copy);
      return -1;
   }
   cJSON *message, *block;
   int failed = 0;
   cJSON_ArrayForEach(message, copy)
   {
      cJSON_ArrayForEach(block, cJSON_GetObjectItemCaseSensitive(message, "blocks"))
      {
         cJSON *context = cJSON_GetObjectItemCaseSensitive(block, "context");
         if (!equal(text(context, "revision_domain"), "native-interruption"))
            continue;
         int found = -1;
         for (int i = 0; i < cJSON_GetArraySize(receipts); i++)
            if (equal(text(cJSON_GetArrayItem(receipts, i), "event_id"),
                      text(context, "revision_scope")))
            {
               found = i;
               break;
            }
         const cJSON *receipt =
             found >= 0
                 ? cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(receipts, found), "receipt")
                 : NULL;
         const cJSON *source = cJSON_GetObjectItemCaseSensitive(receipt, "source");
         aimee_message_t parsed = {0};
         if (found < 0 || seen[found] || !equal(text(message, "role"), "tool") ||
             !equal(text(block, "type"), "tool_result") ||
             !equal(text(block, "tool_id"), text(source, "tool_id")) ||
             !equal(text(block, "content"), warning) ||
             !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(block, "is_error")) ||
             aimee_ir_message_parse(message, &parsed))
         {
            aimee_message_free(&parsed);
            failed = 1;
            break;
         }
         const cJSON *expectedCall =
             cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(receipts, found), "call");
         // The receipt cannot be borrowed by a rewritten call or a later reused ID.
         const cJSON *prior, *candidate;
         const cJSON *pendingCall = NULL;
         cJSON_ArrayForEach(prior, copy)
         {
            if (prior == message)
               break;
            cJSON_ArrayForEach(candidate, cJSON_GetObjectItemCaseSensitive(prior, "blocks"))
            {
               if (!equal(text(candidate, "tool_id"), text(source, "tool_id")))
                  continue;
               if (equal(text(candidate, "type"), "tool_use"))
                  pendingCall = candidate;
               else if (equal(text(candidate, "type"), "tool_result"))
                  pendingCall = NULL;
            }
         }
         if (!pendingCall || !same_call(pendingCall, expectedCall))
         {
            aimee_message_free(&parsed);
            failed = 1;
            break;
         }
         int matching = 0;
         for (int i = 0; i < parsed.n_blocks; i++)
            if (equal(parsed.blocks[i].tool_id, text(source, "tool_id")))
            {
               const aimee_context_meta_t *c = &parsed.blocks[i].context;
               matching = c->origin == AIMEE_CTX_ORIGIN_PLATFORM &&
                          c->authority == AIMEE_CTX_AUTH_DIAGNOSTIC &&
                          c->trust == AIMEE_CTX_TRUST_VERIFIED &&
                          c->sensitivity == AIMEE_CTX_SENS_INTERNAL && c->model_visible;
            }
         aimee_message_free(&parsed);
         if (!matching)
         {
            failed = 1;
            break;
         }
         seen[found] = 1;
         // Only this private admission comparison loses the reserved marker. The
         // model request, source digest, immutable call and durable receipt stay exact.
         if (!cJSON_ReplaceItemInObjectCaseSensitive(
                 context, "revision_domain", cJSON_CreateString("native-interruption-admission")))
         {
            failed = 1;
            break;
         }
      }
      if (failed)
         break;
   }
   int rc = failed ? -1 : aimee_ir_history_admit(copy);
   cJSON_Delete(copy);
   free(seen);
   return rc;
}
int server_native_history_load(const char *session, const char *actor, const char *active_turn,
                               int include_running, cJSON **messages, cJSON **receipts)
{
   if (!messages || !receipts)
      return -1;
   *messages = NULL;
   *receipts = NULL;
   char *wire = NULL;
   int status =
       db1_session_native_history_load(actor, session, include_running ? "running" : "completed",
                                       active_turn ? active_turn : "", &wire);
   cJSON *root = !status && wire ? aimee_ir_json_parse_frame(wire, strlen(wire)) : NULL;
   free(wire);
   if (!exact(root, 2))
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *history = cJSON_DetachItemFromObjectCaseSensitive(root, "messages"),
         *proofs = cJSON_DetachItemFromObjectCaseSensitive(root, "interruptions");
   cJSON_Delete(root);
   int foreign = 0;
   const cJSON *entry;
   cJSON_ArrayForEach(entry, proofs)
   {
      const cJSON *r = cJSON_GetObjectItemCaseSensitive(entry, "receipt");
      const cJSON *source = cJSON_GetObjectItemCaseSensitive(r, "source");
      if (!equal(text(source, "session_id"), session) || !equal(text(source, "actor"), actor))
         foreign = 1;
   }
   if (foreign || server_native_history_admit(history, proofs))
   {
      cJSON_Delete(history);
      cJSON_Delete(proofs);
      return -1;
   }
   *messages = history;
   *receipts = proofs;
   return 0;
}

int server_native_history_admit_source(const char *session, const char *actor,
                                       const char *active_turn, int include_running,
                                       const cJSON *messages)
{
   int reserved = 0;
   const cJSON *message, *block;
   cJSON_ArrayForEach(message, messages) cJSON_ArrayForEach(
       block,
       cJSON_GetObjectItemCaseSensitive(
           message, "blocks")) if (equal(text(cJSON_GetObjectItemCaseSensitive(block, "context"),
                                              "revision_domain"),
                                         "native-interruption")) reserved = 1;
   if (!reserved)
      return aimee_ir_history_admit(messages);
   cJSON *source = NULL, *receipts = NULL;
   if (server_native_history_load(session, actor, active_turn, include_running, &source, &receipts))
      return -1;
   int result = server_native_history_admit(messages, receipts);
   cJSON_Delete(source);
   cJSON_Delete(receipts);
   return result;
}
