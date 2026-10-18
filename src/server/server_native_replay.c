/* Native SDKs do not carry Aimee's local proof on their wire. Retain the proof
 * from actual upstream canonical responses, never stamp a new one on echo. */
#include "server_native_replay.h"
#include "agent_config.h"
#include "db1_client/primary_sessions.h"
#include "vault_principal.h"
#include "aimee_sha256.h"
#include "platform_random.h"
#include "cJSON.h"
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROOF_BUCKETS 1024
#define PROOF_LIMIT   65536

typedef struct proof_entry
{
   struct proof_entry *next;
   char digest[65], binding[199];
   int anthropic, ambiguous;
} proof_entry_t;
struct server_native_replay
{
   pthread_mutex_t lock;
   char *model;
   char session[128], actor[VAULT_PRINCIPAL_MAX], scope[65], turn[33];
   unsigned long long counter;
   unsigned count;
   int loaded;
   proof_entry_t *buckets[PROOF_BUCKETS];
};
static int digest_valid(const char *s)
{
   return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static const char *field(const cJSON *o, const char *key)
{
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
   return s ? s : "";
}
static unsigned proof_bucket(const char *digest)
{
   uint32_t hash = 2166136261u;
   for (unsigned i = 0; i < 64; i++)
      hash = (hash ^ (unsigned char)digest[i]) * 16777619u;
   return hash % PROOF_BUCKETS;
}
static proof_entry_t *find(server_native_replay_t *r, const char *digest, int anthropic)
{
   unsigned bucket = proof_bucket(digest);
   for (proof_entry_t *p = r->buckets[bucket]; p; p = p->next)
      if (p->anthropic == anthropic && !strcmp(p->digest, digest))
         return p;
   return NULL;
}
static int remember(server_native_replay_t *r, const char *digest, const char *binding,
                    int anthropic)
{
   proof_entry_t *p = find(r, digest, anthropic);
   if (p)
   {
      if (strcmp(p->binding, binding))
         p->ambiguous = 1; /* Never select the latest of conflicting prefix proofs. */
      return 0;
   }
   if (r->count == PROOF_LIMIT || !(p = calloc(1, sizeof *p)))
      return -1;
   snprintf(p->digest, sizeof p->digest, "%s", digest);
   snprintf(p->binding, sizeof p->binding, "%s", binding);
   p->anthropic = anthropic;
   unsigned bucket = proof_bucket(digest);
   p->next = r->buckets[bucket];
   r->buckets[bucket] = p;
   r->count++;
   return 0;
}
static int block_digest(const aimee_block_t *block, char digest[65])
{
   cJSON *value = aimee_ir_block_json(block);
   if (!value)
      return -1;
   const char *host[] = {"thinking_binding", "cache_control", "cache_ttl", "message_group",
                         "context"};
   for (unsigned i = 0; i < sizeof host / sizeof host[0]; i++)
      cJSON_DeleteItemFromObjectCaseSensitive(value, host[i]);
   char *wire = cJSON_PrintUnformatted(value);
   cJSON_Delete(value);
   int rc = wire ? aimee_sha256_hex(wire, strlen(wire), digest) : -1;
   free(wire);
   return rc;
}
static int load(server_native_replay_t *r)
{
   if (r->loaded)
      return 0;
   char cursor[32] = "0";
   unsigned long long previous = 0;
   for (unsigned pages = 0; pages < 8192; pages++)
   {
      if (agent_request_cancelled())
         return -1;
      char *raw = db1_session_ir_page(r->session, cursor, 128, r->actor);
      cJSON *page = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
      free(raw);
      if (!cJSON_IsArray(page) || cJSON_GetArraySize(page) > 128)
      {
         cJSON_Delete(page);
         return -1;
      }
      int count = cJSON_GetArraySize(page), rc = 0;
      const cJSON *row;
      cJSON_ArrayForEach(row, page)
      {
         const char *sequence = field(row, "sequence");
         char *end;
         errno = 0;
         unsigned long long n = strtoull(sequence, &end, 10);
         char canonical[32];
         snprintf(canonical, sizeof canonical, "%llu", n);
         if (strcmp(sequence, canonical) || !sequence[0] || errno || *end || n != previous + 1 ||
             strlen(sequence) >= sizeof cursor || strcmp(field(row, "session_id"), r->session) ||
             strcmp(field(row, "actor"), r->actor))
         {
            rc = -1;
            break;
         }
         previous = n;
         snprintf(cursor, sizeof cursor, "%s", sequence);
         const cJSON *event = cJSON_GetObjectItemCaseSensitive(row, "event");
         if (strcmp(field(event, "kind"), "replay_proof"))
            continue;
         const cJSON *proof = cJSON_GetObjectItemCaseSensitive(event, "payload");
         if (strcmp(field(proof, "scope"), r->scope))
            continue;
         const char *digest = field(proof, "block_digest"), *wire = field(proof, "wire");
         const char *binding = field(proof, "thinking_binding");
         int anthropic = !strcmp(wire, "anthropic");
         if (!digest_valid(digest) ||
             (anthropic ? !binding[0] || !aimee_ir_thinking_binding_valid(binding)
                        : strcmp(wire, "responses") ||
                              cJSON_GetObjectItemCaseSensitive(proof, "thinking_binding")) ||
             remember(r, digest, binding, anthropic))
         {
            rc = -1;
            break;
         }
      }
      cJSON_Delete(page);
      if (rc)
         return -1;
      if (count < 128)
      {
         r->loaded = 1;
         return 0;
      }
   }
   return -1;
}
static int append(server_native_replay_t *r, const aimee_session_event_t *event)
{
   cJSON *value = aimee_ir_session_event_json(event);
   char *wire = value ? cJSON_PrintUnformatted(value) : NULL;
   cJSON_Delete(value);
   char id[96], sequence[32];
   snprintf(id, sizeof id, "%s:proof:%llu", r->turn, ++r->counter);
   int rc = wire ? db1_session_ir_append(r->session, id, r->turn, r->actor, wire, sequence) : -1;
   free(wire);
   return rc;
}
int server_native_replay_capture(const aimee_session_event_t *event, void *context)
{
   server_native_replay_t *r = context;
   if (!r || !event)
      return -1;
   pthread_mutex_lock(&r->lock);
   int rc = 0;
   if (event->kind == AIMEE_SESSION_PROJECTION)
      rc = append(r, event);
   else if (event->kind == AIMEE_SESSION_MESSAGE && event->message && event->message->role &&
            !strcmp(event->message->role, "assistant"))
   {
      for (int i = 0; i < event->message->n_blocks && !rc; i++)
      {
         const aimee_block_t *b = &event->message->blocks[i];
         if (b->type != AIMEE_BLK_THINKING)
            continue;
         int anthropic = b->thinking_signature || b->thinking_redacted;
         if (!anthropic && !b->thinking_id && !b->thinking_encrypted_content)
            continue;
         char digest[65];
         if ((anthropic &&
              (!b->thinking_binding || !aimee_ir_thinking_binding_valid(b->thinking_binding))) ||
             block_digest(b, digest))
         {
            rc = -1;
            break;
         }
         aimee_session_event_t proof = {.kind = AIMEE_SESSION_REPLAY_PROOF,
                                        .replay_scope = r->scope,
                                        .replay_digest = digest,
                                        .replay_wire = anthropic ? "anthropic" : "responses",
                                        .replay_binding = anthropic ? b->thinking_binding : NULL};
         rc = append(r, &proof);
         if (!rc)
            rc = remember(r, digest, anthropic ? b->thinking_binding : "", anthropic);
      }
   }
   pthread_mutex_unlock(&r->lock);
   return rc;
}
int server_native_replay_restore(aimee_request_t *request, void *context)
{
   server_native_replay_t *r = context;
   if (!r || !request)
      return -1;
   pthread_mutex_lock(&r->lock);
   int rc = load(r), omitted = 0;
   for (int i = 0; i < request->n_messages && !rc; i++)
      for (int j = 0; j < request->messages[i].n_blocks && !rc; j++)
      {
         aimee_block_t *b = &request->messages[i].blocks[j];
         if (b->type != AIMEE_BLK_THINKING)
            continue;
         if (!b->thinking_signature && !b->thinking_redacted && !b->thinking_id &&
             !b->thinking_encrypted_content)
         {
            free(b->thinking_binding);
            b->thinking_binding = NULL;
            continue;
         }
         int anthropic = request->frontend == AIMEE_WIRE_ANTHROPIC;
         char digest[65];
         if (block_digest(b, digest))
         {
            rc = -1;
            break;
         }
         proof_entry_t *p = find(r, digest, anthropic);
         free(b->thinking_binding);
         b->thinking_binding = NULL;
         if (p && !p->ambiguous)
         {
            if (anthropic && !(b->thinking_binding = strdup(p->binding)))
               rc = -1;
         }
         else
         {
            /* This mutates canonical state, forcing the native renderer to
             * rebuild. Retain reasoning in the durable transcript, never
             * manufacture a new provider identity for unproved opaque state. */
            free(b->thinking_id);
            free(b->thinking_encrypted_content);
            free(b->thinking_signature);
            free(b->thinking_redacted);
            b->thinking_id = b->thinking_encrypted_content = NULL;
            b->thinking_signature = b->thinking_redacted = NULL;
            omitted++;
         }
      }
   if (!rc && omitted)
   {
      aimee_session_event_t report = {.kind = AIMEE_SESSION_PROJECTION,
                                      .projection_model = r->model,
                                      .projection_boundary = "request",
                                      .projection_reasoning = omitted,
                                      .projection_opaque = omitted};
      rc = append(r, &report);
   }
   pthread_mutex_unlock(&r->lock);
   if (rc)
      aimee_ir_session_fail_capture();
   return rc;
}
server_native_replay_t *server_native_replay_open(const char *session, const char *actor,
                                                  const char *scope, const char *model)
{
   if (!session || !session[0] || strlen(session) >= 128 || !actor || !actor[0] ||
       strlen(actor) >= VAULT_PRINCIPAL_MAX || !digest_valid(scope) || !model || !model[0])
      return NULL;
   server_native_replay_t *r = calloc(1, sizeof *r);
   if (!r)
      return NULL;
   if (pthread_mutex_init(&r->lock, NULL))
   {
      free(r);
      return NULL;
   }
   r->model = strdup(model);
   if (!r->model)
   {
      server_native_replay_close(r);
      return NULL;
   }
   unsigned char random[16];
   if (platform_random_bytes(random, sizeof random))
   {
      server_native_replay_close(r);
      return NULL;
   }
   for (unsigned i = 0; i < sizeof random; i++)
      snprintf(r->turn + 2 * i, 3, "%02x", random[i]);
   snprintf(r->session, sizeof r->session, "%s", session);
   snprintf(r->actor, sizeof r->actor, "%s", actor);
   snprintf(r->scope, sizeof r->scope, "%s", scope);
   return r;
}
void server_native_replay_close(server_native_replay_t *r)
{
   if (!r)
      return;
   for (unsigned i = 0; i < PROOF_BUCKETS; i++)
      while (r->buckets[i])
      {
         proof_entry_t *p = r->buckets[i];
         r->buckets[i] = p->next;
         free(p);
      }
   pthread_mutex_destroy(&r->lock);
   free(r->model);
   free(r);
}
