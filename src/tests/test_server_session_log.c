#include "server_session_log.h"
#include "server_session_control.h"
#include "server_session_capture.h"
#include "agent_config.h"
#include "db1_client/server_sessions.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local agent_request_creds_t credentials;
static _Thread_local int job;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct row
{
   char id[128], actor[VAULT_PRINCIPAL_MAX];
   cJSON *events;
   char *history;
} rows[16];
static int count, reject_append, reject_checkpoint, reject_reference, parent_events;
int test_learning_observations(void);
void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   *out = credentials;
}
int agent_get_durable_job_id(void)
{
   return job;
}
int agent_request_cancelled(void)
{
   return 0;
}
static struct row *find(const char *id)
{
   for (int i = 0; i < count; i++)
      if (!strcmp(rows[i].id, id))
         return &rows[i];
   return NULL;
}
void test_learning_assert_source(const cJSON *record)
{
   const char *session =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "session_id"));
   const char *actor = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "actor"));
   int sequence = atoi(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "sequence")));
   pthread_mutex_lock(&lock);
   struct row *row = find(session);
   assert(row && !strcmp(row->actor, actor));
   assert(sequence > 0 && sequence <= cJSON_GetArraySize(row->events));
   assert(cJSON_Compare(cJSON_GetArrayItem(row->events, sequence - 1),
                        cJSON_GetObjectItemCaseSensitive(record, "event"), 1));
   pthread_mutex_unlock(&lock);
}
int db1_server_session_get(const char *id, db1_server_session_t *out)
{
   pthread_mutex_lock(&lock);
   struct row *r = find(id);
   if (r)
   {
      memset(out, 0, sizeof *out);
      snprintf(out->principal, sizeof out->principal, "%s", r->actor);
   }
   pthread_mutex_unlock(&lock);
   return r ? 0 : -1;
}
int db1_server_session_create(const char *id, const char *client, const char *actor)
{
   (void)client;
   pthread_mutex_lock(&lock);
   assert(count < 16 && !find(id));
   struct row *r = &rows[count++];
   snprintf(r->id, sizeof r->id, "%s", id);
   snprintf(r->actor, sizeof r->actor, "%s", actor);
   r->events = cJSON_CreateArray();
   pthread_mutex_unlock(&lock);
   return 0;
}
int db1_session_ir_append(const char *session, const char *id, const char *turn, const char *actor,
                          const char *wire, char sequence[32])
{
   assert(id[0] && turn[0]);
   if (reject_append)
      return -1;
   pthread_mutex_lock(&lock);
   struct row *r = find(session);
   assert(r && !strcmp(r->actor, actor));
   cJSON *event = cJSON_Parse(wire);
   assert(event);
   const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "kind"));
   if (reject_reference && kind && !strcmp(kind, "child_execution"))
   {
      cJSON_Delete(event);
      pthread_mutex_unlock(&lock);
      return -1;
   }
   cJSON_AddItemToArray(r->events, event);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(r->events));
   pthread_mutex_unlock(&lock);
   return 0;
}
int db1_session_ir_history_load(const char *session, const char *scope, const char *actor,
                                const char *active_turn, char **out)
{
   assert(!strcmp(scope, "primary"));
   pthread_mutex_lock(&lock);
   struct row *r = find(session);
   assert(r && !strcmp(r->actor, actor));
   *out = r->history ? strdup(r->history) : NULL;
   pthread_mutex_unlock(&lock);
   return *out ? 0 : 1;
}
int db1_session_ir_history_save(const char *session, const char *checkpoint, const char *scope,
                                const char *actor, const char *wire)
{
   assert(checkpoint[0] && !strcmp(scope, "primary"));
   if (reject_checkpoint)
      return -1;
   pthread_mutex_lock(&lock);
   struct row *r = find(session);
   assert(r && !strcmp(r->actor, actor));
   free(r->history);
   r->history = strdup(wire);
   pthread_mutex_unlock(&lock);
   return 0;
}
static int parent_sink(const aimee_session_event_t *event, void *context)
{
   assert(context == &parent_events && event);
   parent_events++;
   return 0;
}
static void attest(const char *session, const char *actor)
{
   memset(&credentials, 0, sizeof credentials);
   snprintf(credentials.session_id, sizeof credentials.session_id, "%s", session);
   snprintf(credentials.vault_principal, sizeof credentials.vault_principal, "%s", actor);
}
static void model_reply(void)
{
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "canonical reply"};
   aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "model", .message = &message};
   assert(aimee_ir_session_emit(&event) == 0);
}

static void native_tool_identity(void)
{
   const char *native = "opaque:native/call";
   cJSON *input = cJSON_Parse("{\"id\":9007199254740993}");
   assert(input);
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_THINKING, .text = "retained", .thinking_signature = "original-signature"},
       {.type = AIMEE_BLK_TOOL_USE,
        .tool_id = (char *)native,
        .tool_name = "Read",
        .tool_input = input}};
   aimee_response_t response = {
       .role = "assistant", .content = blocks, .n_content = 2, .stop_reason = AIMEE_STOP_TOOL_USE};
   int start = cJSON_GetArraySize(rows[1].events);
   assert(!aimee_ir_session_emit_buffered_response(&response, "native-message"));
   assert(!aimee_ir_session_emit_tool_result(native, "first result", 0));
   aimee_delta_t delta = {.type = AIMEE_DELTA_TURN_START};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA,
                                  .role = "assistant",
                                  .message_id = "native-stream",
                                  .delta = &delta};
   assert(!aimee_ir_session_emit(&event));
   delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_START,
                           .kind = AIMEE_BLK_TOOL_USE,
                           .block_id = 0,
                           .tool_id = native,
                           .tool_name = "Read",
                           .tool_input = input};
   assert(!aimee_ir_session_emit(&event));
   delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_STOP, .block_id = 0};
   assert(!aimee_ir_session_emit(&event));
   aimee_response_t streamed = response;
   streamed.content = &blocks[1];
   streamed.n_content = 1;
   assert(!aimee_ir_session_emit_response(&streamed, "native-stream"));
   delta = (aimee_delta_t){.type = AIMEE_DELTA_TURN_STOP, .stop_reason = AIMEE_STOP_TOOL_USE};
   assert(!aimee_ir_session_emit(&event));
   assert(!aimee_ir_session_emit_tool_result(native, "second result", 0));
   int observed = 0;
   for (int i = start; i < cJSON_GetArraySize(rows[1].events); i++)
   {
      const cJSON *row = cJSON_GetArrayItem(rows[1].events, i),
                  *payload = cJSON_GetObjectItemCaseSensitive(row, "payload");
      const cJSON *list = cJSON_GetObjectItemCaseSensitive(payload, "blocks");
      if (!list)
      {
         const char *id =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "tool_id"));
         if (id)
         {
            assert(!strcmp(id, native));
            observed++;
         }
      }
      else
      {
         const cJSON *block;
         cJSON_ArrayForEach(block, list)
         {
            const char *id =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "tool_id"));
            if (id)
            {
               assert(!strcmp(id, native));
               observed++;
            }
            const char *signature =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "signature"));
            if (signature)
               assert(!strcmp(signature, "original-signature"));
         }
      }
   }
   assert(observed == 5 && !strcmp(blocks[1].tool_id, native));
   cJSON_Delete(input);
}
static void *worker(void *arg)
{
   job = (int)(long)arg;
   attest("parent", "alice"); /* Worker restores a copied, attested request context. */
   server_session_log_t *scope;
   assert(server_session_log_begin("review", "private child prompt", &scope) == 0);
   cJSON *history = NULL;
   assert(server_session_log_history_load(&history) == 1 && !history);
   history = cJSON_Parse(
       "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"private child prompt\"}]}]");
   assert(server_session_log_history_save(history) == 0);
   cJSON_Delete(history);
   model_reply();
   assert(server_session_log_finish(scope, 1, NULL) == 0);
   assert(!server_session_log_active());
   assert(server_session_log_begin("review", "continued", &scope) == 0);
   assert(server_session_log_history_load(&history) == 0 && cJSON_GetArraySize(history) == 1);
   cJSON_Delete(history);
   model_reply();
   assert(server_session_log_finish(scope, 1, NULL) == 0);
   return NULL;
}
static void *fail_capture_worker(void *arg)
{
   server_session_capture_t *capture = arg;
   server_session_capture_fail(capture);
   server_session_capture_release(capture);
   return NULL;
}
static void retained_private_failure_is_terminal(void)
{
   server_session_log_t *scope = NULL;
   int started = server_session_log_begin("review", "private worker failure", &scope);
   assert(!started && scope);
   model_reply();
   char session[128], actor[VAULT_PRINCIPAL_MAX];
   int identified = server_session_log_identity(session, sizeof session, actor, sizeof actor);
   assert(!identified && !strcmp(actor, "alice"));
   server_session_capture_t *capture = NULL;
   int retained = server_session_capture_retain_current("parent", "alice", &capture);
   assert(!retained && capture);
   pthread_t worker;
   int spawned = pthread_create(&worker, NULL, fail_capture_worker, capture);
   assert(!spawned);
   int joined = pthread_join(worker, NULL);
   assert(!joined && !aimee_ir_session_capture_failed());
   int completed = server_session_log_finish(scope, 1, NULL);
   assert(completed < 0 && aimee_ir_session_capture_failed());
   struct row *row = find(session);
   assert(row);
   const cJSON *event = cJSON_GetArrayItem(row->events, cJSON_GetArraySize(row->events) - 1);
   const cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "status")),
                  "failed"));
   aimee_ir_session_set_sink(NULL, NULL);
}

int main(void)
{
   server_session_log_t *scope = NULL;
   assert(server_session_log_begin(NULL, "internal", &scope) == 1 && !scope);
   attest("parent", "");
   assert(server_session_log_begin(NULL, "unattested", &scope) < 0 && !scope);
   attest("parent", "alice");
   aimee_ir_session_set_sink(parent_sink, &parent_events);
   assert(server_session_log_begin(NULL, "existing browser owner", &scope) == 1 && !scope);
   job = 4;
   assert(server_session_log_begin("review", "private", &scope) == 0);
   assert(parent_events == 0 && count == 2 && !strcmp(rows[1].actor, "alice"));
   assert(strcmp(rows[1].id, "parent"));
   char native_scope[128], native_principal[128];
   assert(!server_session_log_identity(native_scope, sizeof native_scope, native_principal,
                                       sizeof native_principal));
   assert(!strcmp(native_scope, rows[1].id) && !strcmp(native_principal, "alice"));
   assert(server_session_log_identity(native_scope, 2, native_principal, sizeof native_principal) <
          0);
   model_reply();
   native_tool_identity();
   assert(server_session_log_finish(scope, 1, NULL) == 0);
   aimee_session_event_t finished = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = "completed"};
   assert(aimee_ir_session_emit(&finished) == 0 && parent_events == 1);
   attest("parent", "mallory");
   assert(server_session_log_begin("review", "wrong parent owner", &scope) < 0 && !scope);
   assert(count == 2);
   attest("parent", "alice");
   int observations_before_rejection = test_learning_observations();
   reject_append = 1;
   assert(server_session_log_begin("review", "stop before execution", &scope) < 0 && scope);
   assert(server_session_log_finish(scope, 0, "failed") < 0);
   assert(aimee_ir_session_capture_failed());
   aimee_ir_session_set_sink(NULL, NULL);
   assert(test_learning_observations() == observations_before_rejection);
   reject_append = 0;
   reject_checkpoint = 1;
   assert(server_session_log_begin("review", "private", &scope) == 0);
   cJSON *empty = cJSON_CreateArray();
   assert(server_session_log_history_save(empty) < 0);
   cJSON_Delete(empty);
   assert(server_session_log_finish(scope, 0, "checkpoint failed") < 0);
   aimee_ir_session_set_sink(NULL, NULL);
   reject_checkpoint = 0;
   job = 6;
   assert(server_session_log_begin("review", "unconverted producer", &scope) == 0);
   assert(server_session_log_finish(scope, 1, NULL) < 0);
   assert(aimee_ir_session_capture_failed());
   aimee_ir_session_set_sink(NULL, NULL);
   reject_reference = 1;
   assert(server_session_log_begin("review", "reference admission must persist", &scope) < 0 &&
          scope);
   reject_reference = 0;
   assert(server_session_log_finish(scope, 0, "reference unavailable") < 0);
   aimee_ir_session_set_sink(NULL, NULL);
   assert(server_session_log_begin("review", "terminal reference", &scope) == 0);
   model_reply();
   reject_reference = 1;
   assert(server_session_log_finish(scope, 1, NULL) < 0);
   assert(aimee_ir_session_capture_failed());
   reject_reference = 0;
   aimee_ir_session_set_sink(NULL, NULL);
   retained_private_failure_is_terminal();
   int before_workers = count;
   int observations_before_workers = test_learning_observations();
   pthread_t threads[2];
   for (long i = 0; i < 2; i++)
      assert(pthread_create(&threads[i], NULL, worker, (void *)(i + 10)) == 0);
   for (int i = 0; i < 2; i++)
      assert(pthread_join(threads[i], NULL) == 0);
   assert(test_learning_observations() == observations_before_workers + 4);
   assert(count == before_workers + 2); /* Each durable job reuses its private child. */
   const cJSON *reference;
   int references = 0;
   cJSON_ArrayForEach(reference, rows[0].events)
   {
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reference, "kind"));
      assert(kind && !strcmp(kind, "child_execution"));
      const cJSON *payload = cJSON_GetObjectItemCaseSensitive(reference, "payload");
      const char *child =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "session_id"));
      assert(child && strcmp(child, "parent") && find(child));
      assert(!cJSON_GetObjectItemCaseSensitive(payload, "prompt"));
      char *wire = cJSON_PrintUnformatted(reference);
      assert(wire && !strstr(wire, "private child prompt") && !strstr(wire, "canonical reply"));
      free(wire);
      references++;
   }
   assert(references >= 10); /* Synchronous and independent concurrent workers. */
   char long_owner[VAULT_PRINCIPAL_MAX] = "webuser:";
   memset(long_owner + 8, 'a', VAULT_CERT_CN_MAX);
   long_owner[8 + VAULT_CERT_CN_MAX] = 0;
   attest("long-parent", long_owner);
   int created = db1_server_session_create("long-parent", "test", long_owner);
   assert(!created);
   assert(!server_session_log_begin("review", "long owner", &scope));
   char exact_actor[VAULT_PRINCIPAL_MAX];
   assert(!server_session_log_identity(native_scope, sizeof native_scope, exact_actor,
                                       sizeof exact_actor));
   assert(!strcmp(exact_actor, long_owner));
   assert(!strcmp(find(native_scope)->actor, long_owner));
   model_reply();
   assert(!server_session_log_finish(scope, 1, NULL));
   long_owner[8 + VAULT_CERT_CN_MAX - 1] = 'b';
   attest("long-parent", long_owner);
   assert(server_session_log_begin("review", "different suffix", &scope) < 0 && !scope);
   aimee_ir_session_set_sink(NULL, NULL);
   for (int i = 0; i < count; i++)
   {
      cJSON_Delete(rows[i].events);
      free(rows[i].history);
   }
   puts("log-only owners: copied attestation, private concurrent children, durable reload and "
        "terminal failures passed");
   return 0;
}
