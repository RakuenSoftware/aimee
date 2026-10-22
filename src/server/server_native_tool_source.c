#include "server_native_tool_source.h"
#include "cJSON.h"
#include "aimee_sha256.h"
#include "db1_client/primary_sessions.h"
#include "db1_client/server_sessions.h"
#include "vault_principal.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_LIMIT 4096
#define SOURCE_BYTES (16u * 1024u * 1024u)
typedef struct source_catalog
{
   struct source_catalog *next;
   aimee_request_t view;
   char digest[65];
} source_catalog_t;
typedef struct source_call
{
   struct source_call *next;
   aimee_request_t original;
   const source_catalog_t *catalog;
   char *message_id;
   char first[32], last[32], digest[65];
   int index, claimed;
} source_call_t;
struct server_native_tool_source
{
   pthread_mutex_t lock;
   server_session_capture_t *capture;
   char source[128], destination[128], actor[VAULT_PRINCIPAL_MAX];
   char source_created[DB1_SS_TS_LEN], destination_created[DB1_SS_TS_LEN];
   size_t bytes;
   unsigned count;
   source_call_t *calls;
   source_catalog_t *catalogs;
   size_t catalog_bytes;
};
static const char *field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}
static int sequence(const char *text, unsigned long long *out)
{
   if (!text || !*text)
      return -1;
   char *end, canonical[32];
   errno = 0;
   unsigned long long n = strtoull(text, &end, 10);
   if (errno || *end || !n || n > LLONG_MAX)
      return -1;
   snprintf(canonical, sizeof canonical, "%llu", n);
   if (strcmp(text, canonical))
      return -1;
   *out = n;
   return 0;
}
static int digest_valid(const char *digest)
{
   return digest && strlen(digest) == 64 && strspn(digest, "0123456789abcdef") == 64;
}
int server_native_tool_source_digest(const char *previous, const cJSON *envelope, char digest[65])
{
   if (!digest)
      return -1;
   /* previous may alias digest for the next event in a response. */
   char before[65] = "";
   if (previous && previous[0])
   {
      if (!digest_valid(previous))
         return -1;
      memcpy(before, previous, sizeof before);
   }
   digest[0] = 0;
   cJSON *view = cJSON_IsObject(envelope) ? aimee_ir_json_canonical_copy(envelope) : NULL;
   char *wire = view ? cJSON_PrintUnformatted(view) : NULL;
   cJSON_Delete(view);
   if (!wire)
      return -1;
   size_t length = strlen(wire);
   char *input = length <= SOURCE_BYTES ? malloc(length + 66) : NULL;
   int rc = -1;
   if (input)
   {
      int n = snprintf(input, length + 66, "%s:%s", before, wire);
      if (n > 0 && (size_t)n < length + 66)
         rc = aimee_sha256_hex(input, (size_t)n, digest);
   }
   free(input);
   free(wire);
   return rc;
}
static int same_json(const cJSON *a, const cJSON *b)
{
   cJSON *left = aimee_ir_json_canonical_copy(a), *right = aimee_ir_json_canonical_copy(b);
   char *x = left ? cJSON_PrintUnformatted(left) : NULL;
   char *y = right ? cJSON_PrintUnformatted(right) : NULL;
   int same = x && y && !strcmp(x, y);
   free(x);
   free(y);
   cJSON_Delete(left);
   cJSON_Delete(right);
   return same;
}
static int same_text(const char *a, const char *b)
{
   return (!a && !b) || (a && b && !strcmp(a, b));
}
static int same_call(const aimee_block_t *a, const aimee_block_t *b)
{
   return a && b && a->type == AIMEE_BLK_TOOL_USE && b->type == AIMEE_BLK_TOOL_USE &&
          same_text(a->tool_id, b->tool_id) && same_text(a->tool_namespace, b->tool_namespace) &&
          same_text(a->tool_name, b->tool_name) && same_json(a->tool_input, b->tool_input);
}
static int incarnation(const char *session, const char *actor, char created[DB1_SS_TS_LEN])
{
   db1_server_session_t row;
   if (db1_server_session_get(session, &row) || strcmp(row.principal, actor) || !row.created_at[0])
      return -1;
   snprintf(created, DB1_SS_TS_LEN, "%s", row.created_at);
   return 0;
}
static int fresh(const server_native_tool_source_t *source)
{
   char a[DB1_SS_TS_LEN], b[DB1_SS_TS_LEN];
   return !incarnation(source->source, source->actor, a) &&
          !incarnation(source->destination, source->actor, b) &&
          !strcmp(a, source->source_created) && !strcmp(b, source->destination_created);
}
server_native_tool_source_t *server_native_tool_source_open(server_session_capture_t *capture,
                                                            const char *session, const char *actor)
{
   const char *destination = server_session_capture_session(capture);
   if (!server_session_capture_accepting(capture) || !destination || !session || !*session ||
       strlen(session) >= 128 || !actor || !*actor || strlen(actor) >= VAULT_PRINCIPAL_MAX)
      return NULL;
   server_native_tool_source_t *source = calloc(1, sizeof *source);
   if (!source)
      return NULL;
   if (pthread_mutex_init(&source->lock, NULL))
   {
      free(source);
      return NULL;
   }
   source->capture = capture;
   snprintf(source->source, sizeof source->source, "%s", session);
   snprintf(source->destination, sizeof source->destination, "%s", destination);
   snprintf(source->actor, sizeof source->actor, "%s", actor);
   if (incarnation(session, actor, source->source_created) ||
       incarnation(destination, actor, source->destination_created))
   {
      server_native_tool_source_close(source);
      return NULL;
   }
   return source;
}
void server_native_tool_source_close(server_native_tool_source_t *source)
{
   if (!source)
      return;
   source_call_t *call = source->calls;
   while (call)
   {
      source_call_t *next = call->next;
      aimee_request_free(&call->original);
      free(call->message_id);
      free(call);
      call = next;
   }
   source_catalog_t *catalog = source->catalogs;
   while (catalog)
   {
      source_catalog_t *next = catalog->next;
      aimee_request_free(&catalog->view);
      free(catalog);
      catalog = next;
   }
   pthread_mutex_destroy(&source->lock);
   free(source);
}
static const source_catalog_t *source_catalog_record(server_native_tool_source_t *source,
                                                     const aimee_request_t *catalog)
{
   aimee_request_t view = {
       .model = catalog->model, .tools = catalog->tools, .n_tools = catalog->n_tools, .mutated = 1};
   cJSON *json = aimee_ir_request_json(&view);
   char *wire = json ? cJSON_PrintUnformatted(json) : NULL;
   cJSON_Delete(json);
   size_t bytes = wire ? strlen(wire) : 0;
   char digest[65];
   int invalid = !bytes || bytes > SOURCE_BYTES || aimee_sha256_hex(wire, bytes, digest);
   free(wire);
   if (invalid)
      return NULL;
   for (source_catalog_t *old = source->catalogs; old; old = old->next)
      if (!strcmp(old->digest, digest))
         return old;
   if (source->catalog_bytes > SOURCE_BYTES - bytes)
      return NULL;
   source_catalog_t *entry = calloc(1, sizeof *entry);
   if (!entry)
      return NULL;
   if (aimee_ir_request_clone(&view, &entry->view))
   {
      free(entry);
      return NULL;
   }
   snprintf(entry->digest, sizeof entry->digest, "%s", digest);
   entry->next = source->catalogs;
   source->catalogs = entry;
   source->catalog_bytes += bytes;
   return entry;
}
static int source_record(server_native_tool_source_t *source, const aimee_request_t *catalog,
                         const aimee_block_t *call, const char *message_id, const char *first,
                         const char *last, const char *digest, int index, int retain_catalog)
{
   unsigned long long start, stop;
   if (!source || !call || !aimee_ir_block_valid(call) || call->type != AIMEE_BLK_TOOL_USE ||
       !cJSON_IsObject(call->tool_input) || !message_id || !*message_id ||
       strlen(message_id) > 1024 || !digest_valid(digest) || sequence(first, &start) ||
       sequence(last, &stop) || start > stop || index < 0 || index >= 4096)
      return -1;
   const aimee_tool_t *function =
       aimee_ir_find_function(catalog, call->tool_namespace, call->tool_name);
   if (!function)
      return -1;
   pthread_mutex_lock(&source->lock);
   int rc = -1;
   if (!server_session_capture_accepting(source->capture))
      goto done;
   for (source_call_t *old = source->calls; old; old = old->next)
      if (old->index == index && !strcmp(old->message_id, message_id))
      {
         /* Full snapshots do not reset an earlier stream call's claim/source. */
         rc = same_call(&old->original.messages[0].blocks[0], call) ? 0 : -1;
         goto done;
      }
   if (source->count == SOURCE_LIMIT)
      goto done;
   source_call_t *entry = calloc(1, sizeof *entry);
   if (!entry)
      goto done;
   aimee_tool_t leaf = *function;
   leaf.raw = NULL;
   aimee_tool_t space = {.kind = AIMEE_TOOL_NAMESPACE,
                         .name = call->tool_namespace,
                         .children = &leaf,
                         .n_children = 1};
   aimee_block_t original = *call;
   original.raw = NULL;
   aimee_message_t message = {.role = "assistant", .blocks = &original, .n_blocks = 1};
   aimee_request_t one = {.model = catalog->model,
                          .messages = &message,
                          .n_messages = 1,
                          .tools = call->tool_namespace ? &space : &leaf,
                          .n_tools = 1,
                          .mutated = 1};
   cJSON *value = aimee_ir_request_json(&one);
   char *wire = value ? cJSON_PrintUnformatted(value) : NULL;
   size_t bytes = wire ? strlen(wire) : 0;
   free(wire);
   cJSON_Delete(value);
   entry->message_id = strdup(message_id);
   if (!bytes || bytes > SOURCE_BYTES || source->bytes > SOURCE_BYTES - bytes ||
       !entry->message_id || aimee_ir_request_clone(&one, &entry->original))
   {
      aimee_request_free(&entry->original);
      free(entry->message_id);
      free(entry);
      goto done;
   }
   if (retain_catalog && !(entry->catalog = source_catalog_record(source, catalog)))
   {
      aimee_request_free(&entry->original);
      free(entry->message_id);
      free(entry);
      goto done;
   }
   snprintf(entry->first, sizeof entry->first, "%s", first);
   snprintf(entry->last, sizeof entry->last, "%s", last);
   snprintf(entry->digest, sizeof entry->digest, "%s", digest);
   entry->index = index;
   entry->next = source->calls;
   source->calls = entry;
   source->count++;
   source->bytes += bytes;
   rc = 0;
done:
   pthread_mutex_unlock(&source->lock);
   return rc;
}
int server_native_tool_source_record(server_native_tool_source_t *source,
                                     const aimee_request_t *catalog, const aimee_block_t *call,
                                     const char *message_id, const char *first, const char *last,
                                     const char *digest, int index)
{
   return source_record(source, catalog, call, message_id, first, last, digest, index, 0);
}
int server_native_tool_source_record_with_catalog(server_native_tool_source_t *source,
                                                  const aimee_request_t *catalog,
                                                  const aimee_block_t *call, const char *message_id,
                                                  const char *first, const char *last,
                                                  const char *digest, int index)
{
   return source_record(source, catalog, call, message_id, first, last, digest, index, 1);
}
static int verify(server_native_tool_source_t *source, const source_call_t *call)
{
   unsigned long long start, stop;
   if (sequence(call->first, &start) || sequence(call->last, &stop))
      return -1;
   unsigned long long previous = start - 1;
   char cursor[32], digest[65] = "";
   size_t bytes = 0;
   for (unsigned pages = 0; pages < 8192; pages++)
   {
      snprintf(cursor, sizeof cursor, "%llu", previous);
      int limit = stop - previous > 128 ? 128 : (int)(stop - previous);
      if (!server_session_capture_accepting(source->capture))
         return -1;
      char *wire = db1_session_ir_page(source->destination, cursor, limit, source->actor);
      size_t size = wire ? strlen(wire) : 0;
      if (!size || size > SOURCE_BYTES || bytes > SOURCE_BYTES - size)
      {
         free(wire);
         return -1;
      }
      bytes += size;
      cJSON *page = aimee_ir_json_parse_frame(wire, size);
      free(wire);
      if (!cJSON_IsArray(page) || !cJSON_GetArraySize(page) || cJSON_GetArraySize(page) > 128)
      {
         cJSON_Delete(page);
         return -1;
      }
      int rc = 0, reached = 0;
      const cJSON *row;
      cJSON_ArrayForEach(row, page)
      {
         unsigned long long n;
         if (sequence(field(row, "sequence"), &n) || n != previous + 1 ||
             strcmp(field(row, "session_id"), source->destination) ||
             strcmp(field(row, "actor"), source->actor))
         {
            rc = -1;
            break;
         }
         previous = n;
         const cJSON *event = cJSON_GetObjectItemCaseSensitive(row, "event");
         if (!strcmp(field(event, "message_id"), call->message_id) &&
             server_native_tool_source_digest(digest, row, digest))
         {
            rc = -1;
            break;
         }
         if (n == stop)
         {
            reached = 1;
            break;
         }
      }
      cJSON_Delete(page);
      if (rc)
         return -1;
      if (reached)
         return !strcmp(digest, call->digest) ? 0 : -1;
   }
   return -1;
}
void server_native_tool_claim_free(server_native_tool_claim_t *claim)
{
   if (claim)
   {
      aimee_request_free(&claim->original);
      aimee_request_free(&claim->catalog);
      memset(claim, 0, sizeof *claim);
   }
}
static int source_claim(server_native_tool_source_t *source, const aimee_block_t *proposed,
                        int reference, server_native_tool_claim_t *out)
{
   if (!source || !out || !proposed || proposed->type != AIMEE_BLK_TOOL_USE || !proposed->tool_id ||
       !proposed->tool_id[0] || !proposed->tool_name || !proposed->tool_name[0] ||
       (proposed->tool_namespace && !proposed->tool_namespace[0]) ||
       (!reference && !aimee_ir_block_valid(proposed)))
      return -1;
   pthread_mutex_lock(&source->lock);
   int rc = -1;
   source_call_t *match = NULL;
   server_native_tool_claim_t result = {0};
   if (!server_session_capture_accepting(source->capture) || !fresh(source))
      goto done;
   unsigned identity_count = 0;
   for (source_call_t *entry = source->calls; entry; entry = entry->next)
   {
      const aimee_block_t *original = &entry->original.messages[0].blocks[0];
      if (reference && same_text(original->tool_id, proposed->tool_id))
         identity_count++;
      int matches = reference ? same_text(original->tool_id, proposed->tool_id) &&
                                    same_text(original->tool_namespace, proposed->tool_namespace) &&
                                    same_text(original->tool_name, proposed->tool_name)
                              : same_call(original, proposed);
      if (!entry->claimed && matches)
      {
         if (match)
            goto done; /* Never choose the newest of ambiguous original calls. */
         match = entry;
      }
   }
   /* A reference has no exact proposed arguments to distinguish reused IDs.
    * Even a previously claimed occurrence makes this identity ambiguous. */
   if (reference && identity_count != 1)
      goto done;
   if (!match || verify(source, match) || !fresh(source) ||
       aimee_ir_request_clone(&match->original, &result.original) ||
       (match->catalog && aimee_ir_request_clone(&match->catalog->view, &result.catalog)) ||
       server_session_capture_claim_call(source->capture, match->last, match->index))
      goto done;
   match->claimed = 1;
   snprintf(result.sequence, sizeof result.sequence, "%s", match->last);
   result.block_index = match->index;
   snprintf(result.digest, sizeof result.digest, "%s", match->digest);
   server_native_tool_claim_free(out);
   *out = result;
   memset(&result, 0, sizeof result);
   rc = 0;
done:
   server_native_tool_claim_free(&result);
   pthread_mutex_unlock(&source->lock);
   return rc;
}

int server_native_tool_source_claim(server_native_tool_source_t *source,
                                    const aimee_block_t *proposed, server_native_tool_claim_t *out)
{
   return source_claim(source, proposed, 0, out);
}
int server_native_tool_source_claim_reference(server_native_tool_source_t *source, const char *id,
                                              const char *space, const char *name,
                                              server_native_tool_claim_t *out)
{
   aimee_block_t reference = {.type = AIMEE_BLK_TOOL_USE,
                              .tool_id = (char *)id,
                              .tool_namespace = (char *)space,
                              .tool_name = (char *)name};
   return source_claim(source, &reference, 1, out);
}
