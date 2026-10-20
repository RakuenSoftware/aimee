/* Native conversation files are a derived cache. The session log is the source;
 * the existing native-handle owner supplies a versioned exclusive claim. */
#include "server_native_cache.h"
#include "server_native_history.h"
#include "agent_config.h"
#include "db1_client/primary_sessions.h"
#include "aimee_sha256.h"
#include "platform_random.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_CACHE_BYTES (16u * 1024u * 1024u)
struct server_native_cache
{
   char session[129], actor[129], turn[129], harness[8];
   char scope[65], context[65], before[65], key[96], metadata[112];
   char claim[33], token[192], native_id[1025];
   char epoch[33], binding[65], owner[33], containment[65];
   int before_count, offset, phase;
};

static const char *field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}
static int hex(const char *value, size_t size)
{
   return value && strlen(value) == size && strspn(value, "0123456789abcdef") == size;
}
static int identity(const char *value)
{
   return value && *value && strlen(value) <= 128;
}
static int token_parts(const char *token, const char *phase, char claim[33], char turn[129])
{
   size_t prefix = strlen(phase);
   if (strncmp(token, phase, prefix) || token[prefix] != ':' || strlen(token + prefix + 1) < 34 ||
       token[prefix + 33] != ':' || !identity(token + prefix + 34))
      return -1;
   memcpy(claim, token + prefix + 1, 32);
   claim[32] = 0;
   if (!hex(claim, 32))
      return -1;
   snprintf(turn, 129, "%s", token + prefix + 34);
   return 0;
}

static int digest(const cJSON *value, char out[65])
{
   cJSON *canonical = aimee_ir_json_canonical_copy(value);
   char *wire = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
   cJSON_Delete(canonical);
   int rc =
       wire && strlen(wire) <= NATIVE_CACHE_BYTES ? aimee_sha256_hex(wire, strlen(wire), out) : -1;
   free(wire);
   return rc;
}
static int prefix_digest(const cJSON *messages, int count, char out[65])
{
   if (!cJSON_IsArray(messages) || count < 0 || count > cJSON_GetArraySize(messages))
      return -1;
   cJSON *prefix = cJSON_CreateArray();
   if (!prefix)
      return -1;
   for (int i = 0; i < count; i++)
   {
      cJSON *copy = cJSON_Duplicate(cJSON_GetArrayItem(messages, i), 1);
      if (!copy || !cJSON_AddItemToArray(prefix, copy))
      {
         cJSON_Delete(copy);
         cJSON_Delete(prefix);
         return -1;
      }
   }
   int rc = digest(prefix, out);
   cJSON_Delete(prefix);
   return rc;
}

static cJSON *source_history(server_native_cache_t *cache, const char *active_turn)
{
   char *wire = NULL;
   int status =
       db1_session_ir_history_load(cache->session, "primary", cache->actor, active_turn, &wire);
   cJSON *source = status == 1           ? cJSON_CreateArray()
                   : status == 0 && wire ? aimee_ir_json_parse_frame(wire, strlen(wire))
                                         : NULL;
   free(wire);
   if (!cJSON_IsArray(source) || cJSON_GetArraySize(source) > AIMEE_IR_MAX_BLOCKS)
   {
      cJSON_Delete(source);
      return NULL;
   }
   /* Validate canonical persisted content without inventing a native fallback. */
   cJSON *normalized = cJSON_CreateArray();
   if (!normalized)
   {
      cJSON_Delete(source);
      return NULL;
   }
   const cJSON *item;
   cJSON_ArrayForEach(item, source)
   {
      aimee_message_t message = {0};
      if (aimee_ir_message_parse(item, &message))
         goto failed;
      cJSON *copy = aimee_ir_message_json(&message);
      aimee_message_free(&message);
      if (!copy || !cJSON_AddItemToArray(normalized, copy))
      {
         cJSON_Delete(copy);
         goto failed;
      }
   }
   if (server_native_history_admit_source(cache->session, cache->actor, active_turn, 0, normalized))
      goto failed;
   cJSON_Delete(source);
   return normalized;
failed:
   cJSON_Delete(source);
   cJSON_Delete(normalized);
   return NULL;
}

static int cache_keys(server_native_cache_t *cache)
{
   char seed[160], key_digest[65];
   snprintf(seed, sizeof seed, "%s:%s:%s", cache->harness, cache->scope, cache->context);
   if (aimee_sha256_hex(seed, strlen(seed), key_digest))
      return -1;
   snprintf(cache->key, sizeof cache->key, "%s-ir-v1-%s", cache->harness, key_digest);
   snprintf(cache->metadata, sizeof cache->metadata, "%s:cache", cache->key);
   return 0;
}

static int checkpoint_valid(const cJSON *value)
{
   const cJSON *count = cJSON_GetObjectItemCaseSensitive(value, "message_count");
   if (!cJSON_IsNumber(count) || count->valuedouble != count->valueint)
      return 0;
   aimee_native_checkpoint_t checkpoint = {.scope = field(value, "scope"),
                                           .context_digest = field(value, "context_digest"),
                                           .prefix_digest = field(value, "prefix_digest"),
                                           .claim = field(value, "claim"),
                                           .harness = field(value, "harness"),
                                           .native_id = field(value, "native_id"),
                                           .message_count = count->valueint};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_NATIVE_CHECKPOINT,
                                  .native_checkpoint = &checkpoint};
   cJSON *encoded = aimee_ir_session_event_json(&event);
   int valid =
       encoded && cJSON_Compare(cJSON_GetObjectItemCaseSensitive(encoded, "payload"), value, 1);
   cJSON_Delete(encoded);
   return valid;
}

/* Validate owner and exact sequence continuity while finding the committed proof
 * and the source turn's lifecycle. A duplicate conflicting proof never wins. */
static int observations(server_native_cache_t *cache, const char *claim, const char *proof_turn,
                        cJSON **proof, int *state, int exclusive_source)
{
   char cursor[32] = "0";
   unsigned long long previous = 0;
   *state = 0;
   *proof = NULL;
   for (unsigned page_index = 0; page_index < 8192; page_index++)
   {
      if (agent_request_cancelled())
         goto failed;
      char *wire = db1_session_ir_page(cache->session, cursor, 128, cache->actor);
      cJSON *page = wire ? aimee_ir_json_parse_frame(wire, strlen(wire)) : NULL;
      free(wire);
      if (!cJSON_IsArray(page) || cJSON_GetArraySize(page) > 128)
      {
         cJSON_Delete(page);
         goto failed;
      }
      int count = cJSON_GetArraySize(page), invalid = 0;
      const cJSON *row;
      cJSON_ArrayForEach(row, page)
      {
         const char *sequence = field(row, "sequence");
         errno = 0;
         char *end;
         unsigned long long n = strtoull(sequence, &end, 10);
         char exact[32];
         snprintf(exact, sizeof exact, "%llu", n);
         if (errno || *end || !*sequence || strcmp(sequence, exact) || n != previous + 1 ||
             strlen(sequence) >= sizeof cursor || strcmp(field(row, "actor"), cache->actor) ||
             strcmp(field(row, "session_id"), cache->session))
         {
            invalid = 1;
            break;
         }
         previous = n;
         snprintf(cursor, sizeof cursor, "%s", sequence);
         const cJSON *event = cJSON_GetObjectItemCaseSensitive(row, "event");
         const cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
         const char *kind = field(event, "kind");
         if (exclusive_source && !strcmp(kind, "native_compaction") &&
             !strcmp(field(row, "turn_id"), cache->turn))
         {
            invalid = 1;
            break;
         }
         if (exclusive_source && *state && strcmp(field(row, "turn_id"), cache->turn) &&
             (!strcmp(kind, "execution") || !strcmp(kind, "message") || !strcmp(kind, "history")))
         {
            invalid = 1;
            break;
         }
         if (!strcmp(field(row, "turn_id"), cache->turn) &&
             !strcmp(field(event, "kind"), "execution"))
         {
            const char *status = field(payload, "status");
            int next = !strcmp(status, "running")                                    ? 1
                       : !strcmp(status, "completed")                                ? 2
                       : (!strcmp(status, "failed") || !strcmp(status, "cancelled")) ? 3
                                                                                     : 0;
            if (!next || *state > 1 || (next == 1 && *state))
            {
               invalid = 1;
               break;
            }
            *state = next;
         }
         if (!claim || strcmp(field(event, "kind"), "native_checkpoint") ||
             strcmp(field(payload, "claim"), claim))
            continue;
         if ((proof_turn && strcmp(field(row, "turn_id"), proof_turn)) ||
             !checkpoint_valid(payload) || (*proof && !cJSON_Compare(*proof, payload, 1)))
         {
            invalid = 1;
            break;
         }
         if (!*proof && !(*proof = cJSON_Duplicate(payload, 1)))
         {
            invalid = 1;
            break;
         }
      }
      cJSON_Delete(page);
      if (invalid)
         goto failed;
      if (count < 128)
         return 0;
   }
failed:
   cJSON_Delete(*proof);
   *proof = NULL;
   return -1;
}

int server_native_cache_epoch_binding(const char *binding, const char *epoch, char out[65])
{
   if (!out || !hex(binding, 64) || !epoch || (strcmp(epoch, "0") && !hex(epoch, 32)))
      return -1;
   if (!strcmp(epoch, "0"))
   {
      snprintf(out, 65, "%s", binding);
      return 0;
   }
   char lineage[160];
   int length =
       snprintf(lineage, sizeof lineage, "aimee-native-cache-epoch-v1:%s:%s", epoch, binding);
   return length < 0 || (size_t)length >= sizeof lineage
              ? -1
              : aimee_sha256_hex(lineage, (size_t)length, out);
}
cJSON *server_native_cache_claim_json(const server_native_cache_t *cache)
{
   if (!cache || !cache->owner[0] || !cache->claim[0])
      return NULL;
   cJSON *value = cJSON_CreateObject();
   if (!value || !cJSON_AddNumberToObject(value, "version", 1) ||
       !cJSON_AddStringToObject(value, "session_id", cache->session) ||
       !cJSON_AddStringToObject(value, "turn_id", cache->turn) ||
       !cJSON_AddStringToObject(value, "harness", cache->harness) ||
       !cJSON_AddStringToObject(value, "scope_sha256", cache->scope) ||
       !cJSON_AddStringToObject(value, "context_sha256", cache->context) ||
       !cJSON_AddStringToObject(value, "binding_sha256", cache->binding) ||
       !cJSON_AddStringToObject(value, "owner_id", cache->owner) ||
       !cJSON_AddStringToObject(value, "claim_id", cache->claim) ||
       !cJSON_AddStringToObject(value, "containment_sha256", cache->containment) ||
       !cJSON_AddStringToObject(value, "epoch", cache->epoch) ||
       !cJSON_AddStringToObject(value, "expected_token", cache->token))
   {
      cJSON_Delete(value);
      return NULL;
   }
   return value;
}
const char *server_native_cache_metadata_key(const server_native_cache_t *cache)
{
   return cache ? cache->metadata : NULL;
}
server_native_cache_t *server_native_cache_begin(const char *session, const char *actor,
                                                 const char *turn, const char *harness,
                                                 const char *scope, const aimee_request_t *request)
{
   return server_native_cache_begin_epoch(session, actor, turn, harness, scope, request, "0", NULL);
}
server_native_cache_t *server_native_cache_begin_epoch(const char *session, const char *actor,
                                                       const char *turn, const char *harness,
                                                       const char *scope,
                                                       const aimee_request_t *request,
                                                       const char *epoch,
                                                       const server_native_cache_owner_t *owner)
{
   if (!epoch || (strcmp(epoch, "0") && !hex(epoch, 32)) ||
       (owner && (!hex(owner->binding, 64) || !hex(owner->owner, 32) ||
                  !hex(owner->containment, 64) || !owner->publish)) ||
       !identity(session) || !identity(actor) || !identity(turn) || !harness ||
       (strcmp(harness, "codex") && strcmp(harness, "claude")) || !hex(scope, 64) || !request ||
       agent_request_cancelled())
      return NULL;
   server_native_cache_t *cache = calloc(1, sizeof *cache);
   if (!cache)
      return NULL;
   snprintf(cache->session, sizeof cache->session, "%s", session);
   snprintf(cache->actor, sizeof cache->actor, "%s", actor);
   snprintf(cache->turn, sizeof cache->turn, "%s", turn);
   snprintf(cache->harness, sizeof cache->harness, "%s", harness);
   snprintf(cache->scope, sizeof cache->scope, "%s", scope);
   snprintf(cache->epoch, sizeof cache->epoch, "%s", epoch);
   if (server_native_cache_epoch_binding(scope, epoch, cache->scope))
   {
      free(cache);
      return NULL;
   }
   if (owner)
   {
      snprintf(cache->binding, sizeof cache->binding, "%s", owner->binding);
      snprintf(cache->owner, sizeof cache->owner, "%s", owner->owner);
      snprintf(cache->containment, sizeof cache->containment, "%s", owner->containment);
   }
   cJSON *canonical = aimee_ir_request_json(request);
   cJSON *messages =
       canonical ? cJSON_DetachItemFromObjectCaseSensitive(canonical, "messages") : NULL;
   cache->before_count = request->n_messages;
   cJSON *source = source_history(cache, turn);
   char source_digest[65];
   int failed = !canonical || !messages || !source || digest(canonical, cache->context) ||
                digest(messages, cache->before) || digest(source, source_digest) ||
                strcmp(cache->before, source_digest);
   cJSON_Delete(source);
   cJSON_Delete(canonical);
   if (failed)
      goto refused;
   if (cache_keys(cache))
      goto refused;
   char prior[1025] = "", native[1025] = "";
   int found = db1_session_harness_get(session, cache->metadata, prior, sizeof prior, actor);
   if (found < 0 || (found == 0 && strncmp(prior, "complete:", 9) && strncmp(prior, "invalid:", 8)))
      goto refused;
   char old_claim[33] = "", old_turn[129] = "";
   int completed = found == 0 && !strncmp(prior, "complete:", 9);
   if (found == 0 && token_parts(prior, completed ? "complete" : "invalid", old_claim, old_turn))
      goto refused;
   int state;
   cJSON *proof = NULL;
   if (observations(cache, completed ? old_claim : NULL, completed ? old_turn : NULL, &proof,
                    &state, 0) ||
       state != 1)
   {
      cJSON_Delete(proof);
      goto refused;
   }
   if (completed)
   {
      int got = db1_session_harness_get(session, cache->key, native, sizeof native, actor);
      if (got || !proof || strcmp(field(proof, "scope"), cache->scope) ||
          strcmp(field(proof, "context_digest"), cache->context) ||
          strcmp(field(proof, "harness"), harness) || strcmp(field(proof, "native_id"), native))
      {
         cJSON_Delete(proof);
         goto refused;
      }
      int count = cJSON_GetObjectItemCaseSensitive(proof, "message_count")->valueint;
      char prefix[65];
      if (count <= request->n_messages && !prefix_digest(messages, count, prefix) &&
          !strcmp(prefix, field(proof, "prefix_digest")))
      {
         snprintf(cache->native_id, sizeof cache->native_id, "%s", native);
         cache->offset = count;
      }
   }
   cJSON_Delete(proof);
   if (platform_random_hex(cache->claim, 32))
      goto refused;
   snprintf(cache->token, sizeof cache->token, "pending:%s:%s", cache->claim, cache->turn);
   if (agent_request_cancelled())
      goto refused;
   if (owner)
   {
      cJSON *claim = server_native_cache_claim_json(cache);
      if (claim)
         cJSON_ReplaceItemInObjectCaseSensitive(claim, "expected_token",
                                                cJSON_CreateString(found == 1 ? "" : prior));
      char *wire = claim ? cJSON_PrintUnformatted(claim) : NULL;
      int failed = !wire || owner->publish(claim, owner->context) ||
                   db1_session_native_owner_claim(actor, wire);
      free(wire);
      cJSON_Delete(claim);
      if (failed)
         goto refused;
   }
   else if (db1_session_harness_compare_swap(session, cache->metadata, found == 1 ? "" : prior,
                                             cache->token, actor))
      goto refused;
   cache->phase = 1;
   cJSON_Delete(messages);
   return cache;
refused:
   cJSON_Delete(messages);
   server_native_cache_close(cache);
   return NULL;
}

const char *server_native_cache_resume(const server_native_cache_t *cache)
{
   return cache && cache->phase == 1 && cache->native_id[0] ? cache->native_id : NULL;
}
int server_native_cache_history_offset(const server_native_cache_t *cache)
{
   return cache ? cache->offset : -1;
}
int server_native_cache_abort(server_native_cache_t *cache)
{
   if (!cache || (cache->phase != 1 && cache->phase != 2))
      return -1;
   char invalid[192];
   snprintf(invalid, sizeof invalid, "invalid:%s:%s", cache->claim, cache->turn);
   if (db1_session_harness_compare_swap(cache->session, cache->metadata, cache->token, invalid,
                                        cache->actor))
      return -1;
   snprintf(cache->token, sizeof cache->token, "%s", invalid);
   cache->phase = 3;
   return 0;
}
void server_native_cache_close(server_native_cache_t *cache)
{
   free(cache);
}

int server_native_cache_complete(server_native_cache_t *cache, const char *native_id)
{
   if (!cache || cache->phase != 1 || !native_id || !*native_id || strlen(native_id) > 1024 ||
       agent_request_cancelled())
      return -1;
   int state;
   cJSON *unused = NULL;
   if (observations(cache, NULL, NULL, &unused, &state, 1) || state != 2)
      return -1;
   cJSON *source = source_history(cache, "");
   char before[65], prefix[65];
   int count = cJSON_GetArraySize(source);
   if (!source || count <= cache->before_count ||
       prefix_digest(source, cache->before_count, before) || strcmp(before, cache->before) ||
       digest(source, prefix))
   {
      cJSON_Delete(source);
      return -1;
   }
   cJSON_Delete(source);
   char finishing[192];
   snprintf(finishing, sizeof finishing, "finishing:%s:%s", cache->claim, cache->turn);
   if (db1_session_harness_compare_swap(cache->session, cache->metadata, cache->token, finishing,
                                        cache->actor))
      return -1;
   snprintf(cache->token, sizeof cache->token, "%s", finishing);
   cache->phase = 2;
   aimee_native_checkpoint_t checkpoint = {.scope = cache->scope,
                                           .context_digest = cache->context,
                                           .prefix_digest = prefix,
                                           .harness = cache->harness,
                                           .native_id = native_id,
                                           .claim = cache->claim,
                                           .message_count = count};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_NATIVE_CHECKPOINT,
                                  .native_checkpoint = &checkpoint};
   cJSON *value = aimee_ir_session_event_json(&event);
   char *wire = value ? cJSON_PrintUnformatted(value) : NULL;
   cJSON_Delete(value);
   char event_id[80], sequence[32];
   snprintf(event_id, sizeof event_id, "native-cache:%s", cache->claim);
   int failed = !wire || db1_session_ir_append(cache->session, event_id, cache->turn, cache->actor,
                                               wire, sequence);
   free(wire);
   if (failed || db1_session_harness_bind(cache->session, cache->key, native_id, cache->actor))
      return -1;
   char completed[192];
   snprintf(completed, sizeof completed, "complete:%s:%s", cache->claim, cache->turn);
   if (db1_session_harness_compare_swap(cache->session, cache->metadata, cache->token, completed,
                                        cache->actor))
      return -1;
   snprintf(cache->token, sizeof cache->token, "%s", completed);
   cache->phase = 3;
   return 0;
}

int server_native_cache_recover(const char *session, const char *actor, const char *turn,
                                const char *harness, const char *scope,
                                const aimee_request_t *request)
{
   if (!identity(session) || !identity(actor) || !identity(turn) || !harness ||
       (strcmp(harness, "codex") && strcmp(harness, "claude")) || !hex(scope, 64) || !request ||
       agent_request_cancelled())
      return -1;
   server_native_cache_t cache = {0};
   snprintf(cache.session, sizeof cache.session, "%s", session);
   snprintf(cache.actor, sizeof cache.actor, "%s", actor);
   snprintf(cache.turn, sizeof cache.turn, "%s", turn);
   snprintf(cache.harness, sizeof cache.harness, "%s", harness);
   snprintf(cache.scope, sizeof cache.scope, "%s", scope);
   cJSON *canonical = aimee_ir_request_json(request);
   if (!canonical)
      return -1;
   cJSON_DeleteItemFromObjectCaseSensitive(canonical, "messages");
   int failed = digest(canonical, cache.context) || cache_keys(&cache);
   cJSON_Delete(canonical);
   if (failed)
      return -1;
   char prior[1025] = "", claim[33], source_turn[129];
   int found = db1_session_harness_get(session, cache.metadata, prior, sizeof prior, actor);
   if (found)
      return found == 1 ? 1 : -1;
   const char *phase = !strncmp(prior, "pending:", 8)      ? "pending"
                       : !strncmp(prior, "finishing:", 10) ? "finishing"
                       : !strncmp(prior, "complete:", 9)   ? "complete"
                                                           : "invalid";
   if (token_parts(prior, phase, claim, source_turn))
      return -1;
   if (!strcmp(phase, "complete") || !strcmp(phase, "invalid"))
      return 1;
   /* A stale recovery caller cannot invalidate a new execution's claim. The
    * caller must separately prove this exact native child is quiescent. */
   if (strcmp(source_turn, turn))
      return -1;
   int state;
   cJSON *unused = NULL;
   if (observations(&cache, NULL, NULL, &unused, &state, 0) || state < 2 ||
       agent_request_cancelled())
      return -1;
   char invalid[192];
   snprintf(invalid, sizeof invalid, "invalid:%s:%s", claim, turn);
   return db1_session_harness_compare_swap(session, cache.metadata, prior, invalid, actor) ? -1 : 0;
}
