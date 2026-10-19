#include "server_native_cache.h"
#include "db1_client/primary_sessions.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   char key[128], value[1025];
} handle_t;
static handle_t handles[32];
static int handle_count, cancelled, history_failure, page_failure, append_failure, bind_failure;
static int cas_calls, cas_failure, cas_ambiguous;
static cJSON *rows, *history;
static char scope[65];
static int handle(const char *key)
{
   for (int i = 0; i < handle_count; i++)
      if (!strcmp(handles[i].key, key))
         return i;
   assert(handle_count < 32);
   int i = handle_count++;
   snprintf(handles[i].key, sizeof handles[i].key, "%s", key);
   return i;
}
int agent_request_cancelled(void)
{
   return cancelled;
}
int db1_session_harness_get(const char *session, const char *key, char *out, size_t size,
                            const char *actor)
{
   assert(!strcmp(session, "s") && !strcmp(actor, "owner"));
   int i = handle(key);
   if (!handles[i].value[0])
      return 1;
   assert(strlen(handles[i].value) < size);
   snprintf(out, size, "%s", handles[i].value);
   return 0;
}
int db1_session_harness_compare_swap(const char *session, const char *key, const char *expected,
                                     const char *replacement, const char *actor)
{
   assert(!strcmp(session, "s") && !strcmp(actor, "owner"));
   int i = handle(key);
   if (++cas_calls == cas_failure)
   {
      if (cas_ambiguous && !strcmp(handles[i].value, expected))
         snprintf(handles[i].value, sizeof handles[i].value, "%s", replacement);
      return -1;
   }
   if (strcmp(handles[i].value, expected))
      return 1;
   snprintf(handles[i].value, sizeof handles[i].value, "%s", replacement);
   return 0;
}
int db1_session_harness_bind(const char *session, const char *key, const char *value,
                             const char *actor)
{
   assert(!strcmp(session, "s") && !strcmp(actor, "owner"));
   if (bind_failure)
      return -1;
   snprintf(handles[handle(key)].value, sizeof handles[0].value, "%s", value);
   return 0;
}
int db1_session_ir_history_load(const char *session, const char *kind, const char *actor,
                                const char *active, char **out)
{
   assert(!strcmp(session, "s") && !strcmp(actor, "owner") && !strcmp(kind, "primary"));
   (void)active;
   *out = history_failure ? NULL : cJSON_PrintUnformatted(history);
   return history_failure ? -1 : 0;
}
char *db1_session_ir_page(const char *session, const char *after, int limit, const char *actor)
{
   assert(!strcmp(session, "s") && !strcmp(actor, "owner") && limit == 128);
   if (page_failure)
      return NULL;
   int start = atoi(after);
   cJSON *page = cJSON_CreateArray();
   for (int i = start; i < cJSON_GetArraySize(rows) && i < start + limit; i++)
      assert(cJSON_AddItemToArray(page, cJSON_Duplicate(cJSON_GetArrayItem(rows, i), 1)));
   char *wire = cJSON_PrintUnformatted(page);
   cJSON_Delete(page);
   return wire;
}
static void record(const char *turn, cJSON *event)
{
   cJSON *row = cJSON_CreateObject();
   char sequence[32];
   snprintf(sequence, sizeof sequence, "%d", cJSON_GetArraySize(rows) + 1);
   cJSON_AddStringToObject(row, "sequence", sequence);
   cJSON_AddStringToObject(row, "actor", "owner");
   cJSON_AddStringToObject(row, "session_id", "s");
   cJSON_AddStringToObject(row, "turn_id", turn);
   cJSON_AddItemToObject(row, "event", event);
   assert(cJSON_AddItemToArray(rows, row));
}
int db1_session_ir_append(const char *session, const char *event_id, const char *turn,
                          const char *actor, const char *wire, char sequence[32])
{
   assert(!strcmp(session, "s") && !strcmp(actor, "owner") &&
          !strncmp(event_id, "native-cache:", 13));
   if (append_failure)
      return -1;
   cJSON *event = cJSON_Parse(wire);
   assert(event);
   record(turn, event);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(rows));
   return 0;
}
static void lifecycle(const char *turn, const char *status)
{
   aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = status};
   cJSON *json = aimee_ir_session_event_json(&event);
   assert(json);
   record(turn, json);
}
static void reset(void)
{
   cJSON_Delete(rows);
   cJSON_Delete(history);
   rows = cJSON_CreateArray();
   history = cJSON_CreateArray();
   memset(handles, 0, sizeof handles);
   handle_count = cancelled = history_failure = page_failure = append_failure = bind_failure =
       cas_calls = cas_failure = cas_ambiguous = 0;
   memset(scope, 'a', 64);
   scope[64] = 0;
}
static void source(const aimee_request_t *request)
{
   cJSON *root = aimee_ir_request_json(request);
   assert(root);
   cJSON_Delete(history);
   history = cJSON_DetachItemFromObjectCaseSensitive(root, "messages");
   cJSON_Delete(root);
   assert(history);
}
static aimee_block_t system_block = {.type = AIMEE_BLK_TEXT, .text = "current policy"};
static aimee_block_t content[] = {{.type = AIMEE_BLK_TEXT, .text = "user"},
                                  {.type = AIMEE_BLK_TEXT, .text = "assistant"},
                                  {.type = AIMEE_BLK_TEXT, .text = "later user"},
                                  {.type = AIMEE_BLK_TEXT, .text = "later assistant"}};
static aimee_message_t messages[] = {{.role = "user", .blocks = content, .n_blocks = 1},
                                     {.role = "assistant", .blocks = content + 1, .n_blocks = 1},
                                     {.role = "user", .blocks = content + 2, .n_blocks = 1},
                                     {.role = "assistant", .blocks = content + 3, .n_blocks = 1}};
static aimee_request_t request(void)
{
   aimee_request_t result = {.model = "model",
                             .frontend = AIMEE_WIRE_RESPONSES,
                             .system = &system_block,
                             .n_system = 1,
                             .messages = messages};
   return result;
}
static server_native_cache_t *begin(aimee_request_t *req, const char *turn)
{
   return server_native_cache_begin("s", "owner", turn, "codex", scope, req);
}
static int recover(const aimee_request_t *req, const char *turn)
{
   return server_native_cache_recover("s", "owner", turn, "codex", scope, req);
}
static void seeded(aimee_request_t *req, const char *native_id)
{
   reset();
   *req = request();
   source(req);
   lifecycle("first", "running");
   server_native_cache_t *cache = begin(req, "first");
   assert(cache && !server_native_cache_resume(cache));
   assert(server_native_cache_complete(cache, native_id) < 0); /* no durable completion */
   req->n_messages = 2;
   source(req);
   lifecycle("first", "completed");
   assert(!server_native_cache_complete(cache, native_id));
   server_native_cache_close(cache);
}
int main(void)
{
   aimee_request_t req;
   seeded(&req, "opaque:native-id");
   lifecycle("second", "running");
   server_native_cache_t *cache = begin(&req, "second");
   assert(cache && !strcmp(server_native_cache_resume(cache), "opaque:native-id"));
   assert(server_native_cache_history_offset(cache) == 2);
   assert(!begin(&req, "second")); /* a second claimant must not use the old proof */
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);
   lifecycle("second", "cancelled");
   lifecycle("third", "running");
   cache = begin(&req, "third");
   assert(cache && !server_native_cache_resume(cache) &&
          !server_native_cache_history_offset(cache));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   seeded(&req, "policy-handle");
   lifecycle("policy", "running");
   system_block.text = "changed policy";
   cache = begin(&req, "policy");
   assert(cache && !server_native_cache_resume(cache));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);
   lifecycle("policy", "failed");
   system_block.text = "current policy";
   lifecycle("unchanged", "running");
   cache = begin(&req, "unchanged");
   assert(cache && !strcmp(server_native_cache_resume(cache), "policy-handle"));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   seeded(&req, "scope-handle");
   lifecycle("scope", "running");
   memset(scope, 'b', 64);
   cache = begin(&req, "scope");
   assert(cache && !server_native_cache_resume(cache));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   seeded(&req, "unseen-tail");
   req.n_messages = 4;
   source(&req);
   lifecycle("tail", "running");
   cache = begin(&req, "tail");
   assert(cache && !strcmp(server_native_cache_resume(cache), "unseen-tail"));
   assert(server_native_cache_history_offset(cache) == 2);
   assert(!strcmp(req.messages[2].blocks[0].text, "later user"));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   seeded(&req, "model-change");
   req.model = "different registered model";
   lifecycle("model", "running");
   cache = begin(&req, "model");
   assert(cache && !server_native_cache_resume(cache));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   seeded(&req, "compacted");
   req.n_messages = 1;
   source(&req);
   lifecycle("compact", "running");
   cache = begin(&req, "compact");
   assert(cache && !server_native_cache_resume(cache));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   seeded(&req, "changed-history");
   lifecycle("edited", "running");
   content[0].text = "edited user";
   source(&req);
   cache = begin(&req, "edited");
   assert(cache && !server_native_cache_resume(cache));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);
   content[0].text = "user";

   seeded(&req, "source");
   lifecycle("source-mismatch", "running");
   req.n_messages = 1;
   assert(!begin(&req, "source-mismatch"));
   req.n_messages = 2;
   history_failure = 1;
   assert(!begin(&req, "source-mismatch"));
   history_failure = 0;
   page_failure = 1;
   assert(!begin(&req, "source-mismatch"));
   page_failure = 0;
   cancelled = 1;
   assert(!begin(&req, "source-mismatch"));
   cancelled = 0;
   cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetArrayItem(rows, 0), "actor",
                                          cJSON_CreateString("foreign"));
   assert(!begin(&req, "source-mismatch"));

   for (int failure = 0; failure < 3; failure++)
   {
      reset();
      req = request();
      source(&req);
      lifecycle("failed-finish", "running");
      cache = begin(&req, "failed-finish");
      assert(cache);
      req.n_messages = 2;
      source(&req);
      lifecycle("failed-finish", "completed");
      append_failure = failure == 0;
      bind_failure = failure == 1;
      cas_failure = failure == 2 ? cas_calls + 2 : 0;
      assert(server_native_cache_complete(cache, "uncertain-handle") < 0);
      lifecycle("next-run", "running");
      assert(!begin(&req, "next-run"));
      append_failure = bind_failure = cas_failure = 0;
      server_native_cache_close(cache); /* host lost the finalization object */
      int recovered = recover(&req, "failed-finish");
      assert(recovered == 0);
      cache = begin(&req, "next-run");
      assert(cache && !server_native_cache_resume(cache));
      assert(!server_native_cache_abort(cache));
      server_native_cache_close(cache);
   }
   seeded(&req, "corrupted-proof");
   lifecycle("corrupt", "running");
   cJSON *proof_row = cJSON_GetArrayItem(rows, 2);
   cJSON *proof_payload = cJSON_GetObjectItemCaseSensitive(
       cJSON_GetObjectItemCaseSensitive(proof_row, "event"), "payload");
   cJSON_ReplaceItemInObjectCaseSensitive(proof_payload, "prefix_digest",
                                          cJSON_CreateString("corrupt"));
   assert(!begin(&req, "corrupt"));

   seeded(&req, "ambiguous-claim");
   lifecycle("ambiguous", "running");
   cas_failure = cas_calls + 1;
   cas_ambiguous = 1;
   assert(!begin(&req, "ambiguous"));
   cas_failure = cas_ambiguous = 0;
   assert(!begin(&req,
                 "ambiguous")); /* committed claim with a lost reply cannot revive the old proof */

   seeded(&req, "unresolved");
   lifecycle("abandoned", "running");
   cache = begin(&req, "abandoned");
   assert(cache);
   server_native_cache_close(cache);
   assert(!begin(&req, "abandoned")); /* close is not evidence that the native child stopped */
   int recovered = recover(&req, "abandoned");
   assert(recovered < 0); /* running source is not recoverable */
   lifecycle("abandoned", "failed");
   page_failure = 1;
   recovered = recover(&req, "abandoned");
   assert(recovered < 0);
   page_failure = 0;
   cas_failure = cas_calls + 1;
   recovered = recover(&req, "abandoned");
   assert(recovered < 0);
   cas_failure = 0;
   recovered = recover(&req, "wrong-turn");
   assert(recovered < 0);
   recovered = recover(&req, "abandoned");
   assert(recovered == 0);
   recovered = recover(&req, "abandoned");
   assert(recovered == 1);
   lifecycle("after-recovery", "running");
   cache = begin(&req, "after-recovery");
   assert(cache && !server_native_cache_resume(cache));
   recovered = recover(&req, "abandoned");
   assert(recovered < 0); /* late recovery cannot invalidate the replacement */
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   /* Recovery invalidates an old object too; a late completion cannot bind it. */
   seeded(&req, "before-recovery");
   lifecycle("late", "running");
   cache = begin(&req, "late");
   assert(cache);
   req.n_messages = 4;
   source(&req);
   lifecycle("late", "completed");
   recovered = recover(&req, "late");
   assert(recovered == 0);
   assert(server_native_cache_complete(cache, "late-native-handle") < 0);
   server_native_cache_close(cache);
   lifecycle("replacement", "running");
   cache = begin(&req, "replacement");
   assert(cache && !server_native_cache_resume(cache));
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   /* A late child cannot checkpoint another turn's completed source history. */
   seeded(&req, "before-other-turn");
   lifecycle("slow", "running");
   cache = begin(&req, "slow");
   assert(cache);
   lifecycle("slow", "completed");
   lifecycle("different", "running");
   req.n_messages = 4;
   source(&req);
   lifecycle("different", "completed");
   int completed = server_native_cache_complete(cache, "did-not-see-other-turn");
   assert(completed < 0);
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);

   reset();
   req = request();
   source(&req);
   lifecycle("failed", "running");
   cache = begin(&req, "failed");
   assert(cache);
   req.n_messages = 2;
   source(&req);
   lifecycle("failed", "failed");
   assert(server_native_cache_complete(cache, "process-exited-zero") < 0);
   assert(!server_native_cache_abort(cache));
   server_native_cache_close(cache);
   cJSON_Delete(rows);
   cJSON_Delete(history);
   puts("native cache: context/prefix/lineage, durable claims, completion and failure refusal "
        "passed");
   return 0;
}
