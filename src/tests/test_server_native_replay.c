#include "server_native_replay.h"
#include "db1_client/primary_sessions.h"
#include "cJSON.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *rows;
static int read_failed, write_failed, corrupt_actor, cancelled;
int agent_request_cancelled(void)
{
   return cancelled;
}
static char scope[65], other_scope[65], binding[199], conflicting[199];
static const char *text(const cJSON *o, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
}
int db1_session_ir_append(const char *session, const char *id, const char *turn, const char *actor,
                          const char *wire, char sequence[32])
{
   assert(!strcmp(session, "native-session") && !strcmp(actor, "alice"));
   if (write_failed)
      return -1;
   cJSON *event = cJSON_Parse(wire), *row = cJSON_CreateObject();
   assert(event && row);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(rows) + 1);
   cJSON_AddStringToObject(row, "session_id", session);
   cJSON_AddStringToObject(row, "sequence", sequence);
   cJSON_AddStringToObject(row, "event_id", id);
   cJSON_AddStringToObject(row, "turn_id", turn);
   cJSON_AddStringToObject(row, "actor", actor);
   cJSON_AddItemToObject(row, "event", event);
   cJSON_AddItemToArray(rows, row);
   return 0;
}
char *db1_session_ir_page(const char *session, const char *cursor, int limit, const char *actor)
{
   assert(!strcmp(session, "native-session") && !strcmp(actor, "alice") && limit == 128);
   if (read_failed)
      return NULL;
   cJSON *page = cJSON_CreateArray();
   int first = atoi(cursor), end = first + limit;
   if (end > cJSON_GetArraySize(rows))
      end = cJSON_GetArraySize(rows);
   for (int i = first; i < end; i++)
   {
      cJSON *row = cJSON_Duplicate(cJSON_GetArrayItem(rows, i), 1);
      if (corrupt_actor)
         cJSON_ReplaceItemInObjectCaseSensitive(row, "actor", cJSON_CreateString("mallory"));
      cJSON_AddItemToArray(page, row);
   }
   char *wire = cJSON_PrintUnformatted(page);
   cJSON_Delete(page);
   return wire;
}
static aimee_request_t request(const aimee_block_t *block, aimee_wire_t wire)
{
   aimee_message_t borrowed = {
       .role = "assistant", .blocks = (aimee_block_t *)block, .n_blocks = 1};
   cJSON *json = aimee_ir_message_json(&borrowed);
   aimee_request_t result = {.frontend = wire, .model = strdup("selected-model"), .n_messages = 1};
   result.messages = calloc(1, sizeof *result.messages);
   assert(json && result.model && result.messages);
   assert(!aimee_ir_message_parse(json, result.messages));
   cJSON_Delete(json);
   return result;
}
static void capture(server_native_replay_t *r, const aimee_block_t *block)
{
   aimee_message_t message = {.role = "assistant", .blocks = (aimee_block_t *)block, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "actual-upstream", .message = &message};
   assert(!server_native_replay_capture(&event, r));
}
static void reset(void)
{
   cJSON_Delete(rows);
   rows = cJSON_CreateArray();
   read_failed = write_failed = corrupt_actor = cancelled = 0;
   aimee_ir_session_set_sink(NULL, NULL);
}
static void test_actual_binding_and_scope(void)
{
   reset();
   aimee_block_t observed = {.type = AIMEE_BLK_THINKING,
                             .text = "source reasoning",
                             .thinking_signature = "source signature",
                             .thinking_binding = binding};
   server_native_replay_t *r =
       server_native_replay_open("native-session", "alice", scope, "selected-model");
   assert(r);
   capture(r, &observed);
   char *log = cJSON_PrintUnformatted(rows);
   assert(log && !strstr(log, "source reasoning") && !strstr(log, "source signature"));
   assert(strstr(log, "replay_proof") && strstr(log, binding));
   free(log);
   server_native_replay_close(r);
   r = server_native_replay_open("native-session", "alice", scope, "selected-model");
   aimee_request_t q = request(&observed, AIMEE_WIRE_ANTHROPIC);
   free(q.messages[0].blocks[0].thinking_binding);
   q.messages[0].blocks[0].thinking_binding = strdup(conflicting);
   q.messages[0].blocks[0].cache_control = strdup("ephemeral");
   assert(!server_native_replay_restore(&q, r));
   assert(!strcmp(q.messages[0].blocks[0].thinking_binding, binding));
   /* Echoed/edited content cannot acquire the old block's proof. */
   free(q.messages[0].blocks[0].text);
   q.messages[0].blocks[0].text = strdup("edited reasoning");
   assert(!server_native_replay_restore(&q, r));
   assert(!q.messages[0].blocks[0].thinking_binding);
   aimee_request_free(&q);
   server_native_replay_close(r);
   r = server_native_replay_open("native-session", "alice", other_scope, "selected-model");
   q = request(&observed, AIMEE_WIRE_ANTHROPIC);
   assert(!server_native_replay_restore(&q, r) && !q.messages[0].blocks[0].thinking_binding);
   aimee_request_free(&q);
   server_native_replay_close(r);
   /* Conflicting actual proofs do not become last-write-wins. */
   r = server_native_replay_open("native-session", "alice", scope, "selected-model");
   observed.thinking_binding = conflicting;
   capture(r, &observed);
   q = request(&observed, AIMEE_WIRE_ANTHROPIC);
   assert(!server_native_replay_restore(&q, r) && !q.messages[0].blocks[0].thinking_binding);
   aimee_request_free(&q);
   server_native_replay_close(r);
}
static void test_responses_opaque_state(void)
{
   reset();
   aimee_block_t observed = {.type = AIMEE_BLK_THINKING,
                             .text = "summary",
                             .thinking_id = "rs_observed",
                             .thinking_encrypted_content = "encrypted-owned-state"};
   server_native_replay_t *r =
       server_native_replay_open("native-session", "alice", scope, "selected-model");
   capture(r, &observed);
   aimee_request_t q = request(&observed, AIMEE_WIRE_RESPONSES);
   assert(!server_native_replay_restore(&q, r));
   assert(!strcmp(q.messages[0].blocks[0].thinking_id, "rs_observed"));
   assert(!strcmp(q.messages[0].blocks[0].thinking_encrypted_content, "encrypted-owned-state"));
   aimee_request_free(&q);
   server_native_replay_close(r);
   r = server_native_replay_open("native-session", "alice", other_scope, "selected-model");
   q = request(&observed, AIMEE_WIRE_RESPONSES);
   assert(!server_native_replay_restore(&q, r));
   assert(!q.messages[0].blocks[0].thinking_id &&
          !q.messages[0].blocks[0].thinking_encrypted_content);
   assert(!strcmp(q.messages[0].blocks[0].text, "summary"));
   int reports = cJSON_GetArraySize(rows);
   assert(!server_native_replay_restore(&q, r));
   assert(cJSON_GetArraySize(rows) == reports); /* Entry/egress recheck reports once. */
   const cJSON *event = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 1), "event");
   assert(!strcmp(text(event, "kind"), "projection"));
   assert(!strcmp(text(cJSON_GetObjectItemCaseSensitive(event, "payload"), "model"),
                  "selected-model"));
   assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(event, "payload"),
                                           "omitted_opaque_state_blocks")
              ->valueint == 1);
   aimee_request_free(&q);
   server_native_replay_close(r);
}
static void *capture_worker(void *context)
{
   aimee_block_t block = {.type = AIMEE_BLK_THINKING,
                          .text = "reasoning",
                          .thinking_signature = "signature",
                          .thinking_binding = binding};
   for (int i = 0; i < 20; i++)
      capture(context, &block);
   return NULL;
}
static void test_paging_concurrency_and_failures(void)
{
   reset();
   server_native_replay_t *r =
       server_native_replay_open("native-session", "alice", scope, "selected-model");
   pthread_t workers[8];
   for (int i = 0; i < 8; i++)
      assert(!pthread_create(&workers[i], NULL, capture_worker, r));
   for (int i = 0; i < 8; i++)
      pthread_join(workers[i], NULL);
   assert(cJSON_GetArraySize(rows) == 160);
   server_native_replay_close(r);
   r = server_native_replay_open("native-session", "alice", scope, "selected-model");
   aimee_block_t observed = {.type = AIMEE_BLK_THINKING,
                             .text = "reasoning",
                             .thinking_signature = "signature",
                             .thinking_binding = binding};
   aimee_request_t q = request(&observed, AIMEE_WIRE_ANTHROPIC);
   assert(!server_native_replay_restore(&q, r));
   assert(!strcmp(q.messages[0].blocks[0].thinking_binding, binding));
   aimee_request_free(&q);
   server_native_replay_close(r);
   for (int failure = 0; failure < 3; failure++)
   {
      aimee_ir_session_set_sink(NULL, NULL);
      r = server_native_replay_open("native-session", "alice", scope, "selected-model");
      aimee_ir_session_set_sink(server_native_replay_capture, r);
      read_failed = failure == 0;
      corrupt_actor = failure == 1;
      cancelled = failure == 2;
      q = request(&observed, AIMEE_WIRE_ANTHROPIC);
      assert(server_native_replay_restore(&q, r) == -1 && aimee_ir_session_capture_failed());
      aimee_request_free(&q);
      aimee_ir_session_set_sink(NULL, NULL);
      server_native_replay_close(r);
   }
   read_failed = corrupt_actor = cancelled = 0;
   r = server_native_replay_open("native-session", "alice", scope, "selected-model");
   write_failed = 1;
   aimee_ir_session_set_sink(server_native_replay_capture, r);
   aimee_message_t message = {.role = "assistant", .blocks = &observed, .n_blocks = 1};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_MESSAGE, .message = &message};
   assert(aimee_ir_session_emit(&event) == -1 && aimee_ir_session_capture_failed());
   aimee_ir_session_set_sink(NULL, NULL);
   server_native_replay_close(r);
}
int main(void)
{
   memset(scope, 'a', 64);
   memset(other_scope, 'b', 64);
   snprintf(binding, sizeof binding, "ap1:%s:%s:%s", scope, scope, scope);
   snprintf(conflicting, sizeof conflicting, "ap1:%s:%s:%s", other_scope, scope, scope);
   assert(!server_native_replay_open("", "alice", scope, "selected-model"));
   assert(!server_native_replay_open("native-session", "", scope, "selected-model"));
   assert(!server_native_replay_open("native-session", "alice", "bad-scope", "selected-model"));
   test_actual_binding_and_scope();
   test_responses_opaque_state();
   test_paging_concurrency_and_failures();
   cJSON_Delete(rows);
   puts("native replay: observed proof, scope, ambiguity, omission, paging, concurrency and I/O "
        "failures passed");
   return 0;
}

/* This ordinary fixture has no registered native resource producer. */
int db1_session_native_source_append(const char *session, const char *id, const char *turn,
                                     const char *actor, const char *payload, char sequence[32])
{
   (void)session;
   (void)id;
   (void)turn;
   (void)actor;
   (void)payload;
   (void)sequence;
   assert(!"unexpected native owner in ordinary capture fixture");
   return -1;
}
