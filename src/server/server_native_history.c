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

/* Host source metadata is bounded independently of provider-visible bytes. */
#define TOOL_SOURCE_META_MAX (8u * 1024u * 1024u)
extern int aimee_tool_source_visible_valid(const cJSON *, const char *) __attribute__((weak));
extern int aimee_module_history_tool_source_accept(const cJSON *, const char *)
    __attribute__((weak));
extern int aimee_module_tool_source_verify(const cJSON *, const char *) __attribute__((weak));
typedef struct
{
   const char *id, *owner, *digest, *receipt, *visible;
} tool_source_key_t;
static int source_key(const aimee_block_t *block, tool_source_key_t *key)
{
   if (!block || block->type != AIMEE_BLK_TOOL_RESULT || !block->tool_id || !block->tool_id[0] ||
       !aimee_ir_tool_source_shape_valid(block->tool_source) || !cJSON_IsString(block->tool_result))
      return -1;
   *key = (tool_source_key_t){block->tool_id, text(block->tool_source, "owner"),
                              text(block->tool_source, "visible_sha256"),
                              text(block->tool_source, "receipt"),
                              cJSON_GetStringValue(block->tool_result)};
   return 0;
}
static int key_compare(const void *left, const void *right)
{
   const tool_source_key_t *a = left, *b = right;
   int order = strcmp(a->id, b->id);
   if (!order)
      order = strcmp(a->owner, b->owner);
   if (!order)
      order = strcmp(a->digest, b->digest);
   if (!order)
      order = strcmp(a->receipt, b->receipt);
   return order;
}
static int request_source_bounds(const aimee_request_t *request, size_t *count)
{
   if (!request || request->n_messages < 0 || request->n_messages > AIMEE_IR_MAX_BLOCKS ||
       (request->n_messages && !request->messages))
      return -1;
   size_t bytes = 0;
   *count = 0;
   for (int i = 0; i < request->n_messages; i++)
   {
      const aimee_message_t *message = &request->messages[i];
      if (message->n_blocks < 0 || message->n_blocks > AIMEE_IR_MAX_BLOCKS ||
          (message->n_blocks && !message->blocks))
         return -1;
      for (int j = 0; j < message->n_blocks; j++)
         if (message->blocks[j].tool_source)
         {
            tool_source_key_t key;
            if (source_key(&message->blocks[j], &key) || ++*count > AIMEE_IR_MAX_BLOCKS)
               return -1;
            size_t add =
                strlen(key.id) + strlen(key.owner) + strlen(key.digest) + strlen(key.receipt);
            if (add > TOOL_SOURCE_META_MAX || bytes > TOOL_SOURCE_META_MAX - add)
               return -1;
            bytes += add;
         }
   }
   return 0;
}

/* Deployment classification is DENY-only. It never turns a tool name or text
 * into source authority; real owner verification still handles every receipt. */
extern int aimee_module_tool_source_mode(const char *) __attribute__((weak));
extern int aimee_sha256_hex(const void *, size_t, char[65]) __attribute__((weak));
typedef struct
{
   const char *id;
   char call_hash[65];
   int pending, required, any_required;
} required_source_id_t;
typedef struct
{
   const char *id;
   char call_hash[65], result_hash[65];
   unsigned flags;
} required_source_result_t;
static int required_id_compare(const void *left, const void *right)
{
   return strcmp(((const required_source_id_t *)left)->id,
                 ((const required_source_id_t *)right)->id);
}
static int required_result_compare(const void *left, const void *right)
{
   const required_source_result_t *a = left, *b = right;
   int order = strcmp(a->id, b->id);
   if (!order)
      order = strcmp(a->call_hash, b->call_hash);
   if (!order)
      order = strcmp(a->result_hash, b->result_hash);
   return order;
}
static int required_json_hash(const cJSON *value, char digest[65], size_t *work_bytes)
{
   cJSON *canonical = value ? aimee_ir_json_canonical_copy(value) : cJSON_CreateNull();
   char *wire = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
   cJSON_Delete(canonical);
   if (!wire)
      return -1;
   size_t bytes = strlen(wire);
   int invalid =
       bytes > TOOL_SOURCE_META_MAX * 2u || *work_bytes > TOOL_SOURCE_META_MAX * 2u - bytes;
   if (!invalid)
   {
      *work_bytes += bytes;
      invalid = aimee_sha256_hex(wire, bytes, digest) != 0;
   }
   free(wire);
   return invalid ? -1 : 0;
}
/* Match delivered tool bytes, not the JSON type carrying them. Provider
 * codecs unwrap strings and compact-print nonstrings; a string containing
 * JSON and that equivalent object must not evade source custody. */
static int required_visible_hash(const cJSON *value, char digest[65], size_t *work_bytes)
{
   char *owned = NULL;
   const char *wire = cJSON_IsString(value) ? value->valuestring : NULL;
   if (!wire)
   {
      owned = value ? cJSON_PrintUnformatted(value) : strdup("null");
      wire = owned;
   }
   if (!wire)
      return -1;
   size_t bytes = strlen(wire);
   int invalid =
       bytes > TOOL_SOURCE_META_MAX * 2u || *work_bytes > TOOL_SOURCE_META_MAX * 2u - bytes;
   if (!invalid)
   {
      *work_bytes += bytes;
      invalid = aimee_sha256_hex(wire, bytes, digest) != 0;
   }
   free(owned);
   return invalid ? -1 : 0;
}
static int body_hash_compare(const void *left, const void *right)
{
   return strcmp(left, right);
}
static int body_hash_pointer_compare(const void *left, const void *right)
{
   return strcmp(*(const char *const *)left, *(const char *const *)right);
}
static int required_call_hash(const aimee_block_t *call, char digest[65], size_t *work_bytes)
{
   cJSON *identity = cJSON_CreateObject();
   cJSON *input = call->tool_input ? cJSON_Duplicate(call->tool_input, 1) : cJSON_CreateNull();
   if (!identity || !input ||
       !cJSON_AddStringToObject(identity, "name", call->tool_name ? call->tool_name : "") ||
       !cJSON_AddStringToObject(identity, "namespace",
                                call->tool_namespace ? call->tool_namespace : ""))
   {
      cJSON_Delete(identity);
      cJSON_Delete(input);
      return -1;
   }
   if (!cJSON_AddItemToObject(identity, "input", input))
   {
      cJSON_Delete(input);
      cJSON_Delete(identity);
      return -1;
   }
   int result = required_json_hash(identity, digest, work_bytes);
   cJSON_Delete(identity);
   return result;
}
/* A namespace is part of canonical tool identity. Never classify an unrelated
 * business tool merely because its local name resembles a selected host tool. */
static int required_tool_mode(const aimee_block_t *block)
{
   if (!aimee_module_tool_source_mode || !block->tool_name)
      return 0;
   if (!block->tool_namespace || !block->tool_namespace[0])
      return aimee_module_tool_source_mode(block->tool_name);
   size_t space = strnlen(block->tool_namespace, 1025);
   size_t name = strnlen(block->tool_name, 1025);
   if (space > 1024 || name > 1024 || space + name + 2 > 2048)
      return -1;
   char qualified[2048];
   memcpy(qualified, block->tool_namespace, space);
   qualified[space] = '.';
   memcpy(qualified + space + 1, block->tool_name, name + 1);
   return aimee_module_tool_source_mode(qualified);
}
static int required_tool_sources_preserved(const aimee_request_t *canonical,
                                           const aimee_request_t *retained)
{
   if (!aimee_module_tool_source_mode)
      return 0; /* Unconfigured legacy deployment. */
   required_source_id_t *ids = calloc(AIMEE_IR_MAX_BLOCKS * 2u, sizeof *ids);
   required_source_result_t *results = NULL;
   const char **protected_bodies = NULL;
   size_t protected_count = 0;
   if (!ids)
      return -1;
   size_t count = 0, unique = 0, result_count = 0, work_bytes = 0;
   const aimee_request_t *views[] = {canonical, retained};
   int failed = 0;
   for (size_t v = 0; v < 2 && !failed; v++)
      for (int i = 0; i < views[v]->n_messages && !failed; i++)
         for (int j = 0; j < views[v]->messages[i].n_blocks; j++)
         {
            const aimee_block_t *block = &views[v]->messages[i].blocks[j];
            if (block->type != AIMEE_BLK_TOOL_USE)
               continue;
            if (!block->tool_id || !block->tool_id[0] || !block->tool_name)
            {
               failed = 1;
               break;
            }
            if (!required_tool_mode(block))
               continue;
            if (count >= AIMEE_IR_MAX_BLOCKS * 2u)
            {
               failed = 1;
               break;
            }
            ids[count++].id = block->tool_id;
         }
   if (failed || !count)
   {
      free(ids);
      return failed ? -1 : 0;
   }
   if (!aimee_sha256_hex)
   {
      free(ids);
      return -1;
   }
   qsort(ids, count, sizeof *ids, required_id_compare);
   for (size_t i = 0; i < count; i++)
      if (!unique || strcmp(ids[i].id, ids[unique - 1].id))
         ids[unique++] = ids[i];
   if (unique > AIMEE_IR_MAX_BLOCKS)
   {
      free(ids);
      return -1;
   }
   results = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *results);
   if (!results)
   {
      free(ids);
      return -1;
   }
   protected_bodies = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *protected_bodies);
   if (!protected_bodies)
   {
      free(results);
      free(ids);
      return -1;
   }
   for (size_t v = 0; v < 2 && !failed; v++)
   {
      if (v)
      {
         for (size_t i = 0; i < unique; i++)
         {
            ids[i].pending = 0;
            ids[i].required = 0;
            ids[i].call_hash[0] = 0;
         }
         qsort(results, result_count, sizeof *results, required_result_compare);
         size_t kept = 0;
         for (size_t i = 0; i < result_count; i++)
         {
            if (kept && !required_result_compare(&results[i], &results[kept - 1]))
               results[kept - 1].flags |= results[i].flags;
            else
               results[kept++] = results[i];
         }
         result_count = kept;
         for (size_t k = 0; k < result_count; k++)
            if (results[k].flags & 1u)
               protected_bodies[protected_count++] = results[k].result_hash;
         qsort(protected_bodies, protected_count, sizeof *protected_bodies,
               body_hash_pointer_compare);
      }
      for (int i = 0; i < views[v]->n_messages && !failed; i++)
         for (int j = 0; j < views[v]->messages[i].n_blocks && !failed; j++)
         {
            const aimee_block_t *block = &views[v]->messages[i].blocks[j];
            if (block->type != AIMEE_BLK_TOOL_USE && block->type != AIMEE_BLK_TOOL_RESULT)
               continue;
            if (!block->tool_id || !block->tool_id[0])
            {
               failed = 1;
               break;
            }
            char retained_hash[65] = {0};
            int retained_hashed = 0;
            if (v && block->type == AIMEE_BLK_TOOL_RESULT && !block->tool_source && protected_count)
            {
               if (required_visible_hash(block->tool_result, retained_hash, &work_bytes))
               {
                  failed = 1;
                  break;
               }
               retained_hashed = 1;
               const char *wanted = retained_hash;
               if (bsearch(&wanted, protected_bodies, protected_count, sizeof *protected_bodies,
                           body_hash_pointer_compare))
               {
                  failed = 1;
                  break;
               }
            }
            required_source_id_t query = {.id = block->tool_id};
            required_source_id_t *slot =
                bsearch(&query, ids, unique, sizeof *ids, required_id_compare);
            if (!slot)
               continue; /* Ordinary unrelated business tool. */
            if (block->type == AIMEE_BLK_TOOL_USE)
            {
               if (slot->pending || !block->tool_name ||
                   required_call_hash(block, slot->call_hash, &work_bytes))
               {
                  failed = 1;
                  break;
               }
               slot->pending = 1;
               slot->required = required_tool_mode(block) != 0;
               continue;
            }
            if (!slot->pending)
            {
               failed = 1;
               break;
            }
            slot->pending = 0;
            if (v && block->tool_source)
               continue; /* Owner checks, not this classifier, decide trust. */
            if (v && slot->required)
            {
               failed = 1;
               break;
            }
            required_source_result_t key = {.id = block->tool_id};
            memcpy(key.call_hash, slot->call_hash, sizeof key.call_hash);
            if (retained_hashed)
               memcpy(key.result_hash, retained_hash, sizeof key.result_hash);
            else if (required_visible_hash(block->tool_result, key.result_hash, &work_bytes))
            {
               failed = 1;
               break;
            }
            if (!v)
            {
               if (result_count >= AIMEE_IR_MAX_BLOCKS)
               {
                  failed = 1;
                  break;
               }
               key.flags = slot->required ? 1u : 2u;
               results[result_count++] = key;
               slot->any_required |= slot->required;
            }
            else
            {
               const required_source_result_t *found =
                   result_count ? bsearch(&key, results, result_count, sizeof *results,
                                          required_result_compare)
                                : NULL;
               if ((found && (found->flags & 1u)) || (!found && slot->any_required))
                  failed = 1;
            }
         }
   }
   free(protected_bodies);
   free(results);
   free(ids);
   return failed ? -1 : 0;
}

int server_native_history_tool_sources_preserved(const aimee_request_t *canonical,
                                                 const aimee_request_t *retained)
{
   if (!aimee_ir_original_inputs_preserved(canonical, retained))
      return -1;
   size_t count, retained_count;
   if (request_source_bounds(canonical, &count) || request_source_bounds(retained, &retained_count))
      return -1;
   if (required_tool_sources_preserved(canonical, retained))
      return -1;
   tool_source_key_t *keys = count ? calloc(count, sizeof *keys) : NULL;
   if (count && !keys)
      return -1;
   char(*bodies)[65] = count ? calloc(count, sizeof *bodies) : NULL;
   if (count && !bodies)
   {
      free(keys);
      return -1;
   }
   size_t body_work = 0;
   size_t n = 0;
   for (int i = 0; i < canonical->n_messages; i++)
      for (int j = 0; j < canonical->messages[i].n_blocks; j++)
         if (canonical->messages[i].blocks[j].tool_source)
         {
            (void)source_key(&canonical->messages[i].blocks[j], &keys[n]);
            if (required_visible_hash(canonical->messages[i].blocks[j].tool_result, bodies[n],
                                      &body_work))
            {
               free(bodies);
               free(keys);
               return -1;
            }
            n++;
         }
   if (count)
   {
      qsort(keys, count, sizeof *keys, key_compare);
      qsort(bodies, count, sizeof *bodies, body_hash_compare);
   }
   int failed = 0;
   for (int i = 0; i < retained->n_messages && !failed; i++)
      for (int j = 0; j < retained->messages[i].n_blocks && !failed; j++)
      {
         const aimee_block_t *block = &retained->messages[i].blocks[j];
         if (block->type != AIMEE_BLK_TOOL_RESULT)
            continue;
         if (block->tool_source)
         {
            tool_source_key_t key;
            if (source_key(block, &key))
            {
               failed = 1;
               break;
            }
            const tool_source_key_t *found =
                count ? bsearch(&key, keys, count, sizeof *keys, key_compare) : NULL;
            if (!found || strcmp(found->visible, key.visible))
               failed = 1;
         }
         else
         {
            char body[65];
            if (count && (required_visible_hash(block->tool_result, body, &body_work) ||
                          bsearch(body, bodies, count, sizeof *bodies, body_hash_compare)))
            {
               failed = 1;
               break;
            }
            if (!block->tool_id)
               continue;
            /* A retained result with an originally sourced call ID cannot discard
             * or rewrite its receipt. Ambiguous reused IDs fail conservatively. */
            size_t lo = 0, hi = count;
            while (lo < hi)
            {
               size_t mid = lo + (hi - lo) / 2;
               if (strcmp(keys[mid].id, block->tool_id) < 0)
                  lo = mid + 1;
               else
                  hi = mid;
            }
            if (lo < count && !strcmp(keys[lo].id, block->tool_id))
               failed = 1;
         }
      }
   free(bodies);
   free(keys);
   return failed ? -1 : 0;
}
static int message_tool_sources(const aimee_message_t *message, int import)
{
   aimee_request_t request = {.messages = (aimee_message_t *)message, .n_messages = 1};
   size_t count;
   if (request_source_bounds(&request, &count))
      return -1;
   for (int i = 0; i < message->n_blocks; i++)
   {
      const aimee_block_t *block = &message->blocks[i];
      if (!block->tool_source)
         continue;
      const char *visible = cJSON_GetStringValue(block->tool_result);
      if (!aimee_tool_source_visible_valid ||
          !aimee_tool_source_visible_valid(block->tool_source, visible))
         return -1;
      if (import)
      {
         if (!aimee_module_history_tool_source_accept ||
             aimee_module_history_tool_source_accept(block->tool_source, visible))
            return -1;
      }
      else if (!aimee_module_tool_source_verify ||
               aimee_module_tool_source_verify(block->tool_source, visible))
         return -1;
   }
   return 0;
}
int server_native_history_verify_message_tool_sources(const aimee_message_t *message)
{
   return message_tool_sources(message, 0);
}
int server_native_history_accept_message_tool_sources(const aimee_message_t *message)
{
   return message_tool_sources(message, 1);
}
int server_native_history_accept_tool_sources(const aimee_request_t *request)
{
   size_t count;
   if (request_source_bounds(request, &count))
      return -1;
   for (int i = 0; i < request->n_messages; i++)
      if (server_native_history_accept_message_tool_sources(&request->messages[i]))
         return -1;
   return 0;
}
