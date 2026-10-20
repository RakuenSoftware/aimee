#include "server_session_log.h"
#include "server_session_control.h"
#include "server_session_capture.h"
#include "agent_config.h"
#include "server_learning_outcome.h"
#include "util.h"
#include "db1_client/server_sessions.h"
#include "db1_client/agent_jobs.h"
#include "db1_client/primary_sessions.h"
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
   cJSON *events, *envelopes;
   char *history;
} rows[32];
static int count, reject_append, reject_checkpoint, reject_reference, parent_events, history_fault;
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
   assert(count < 32 && !find(id));
   struct row *r = &rows[count++];
   snprintf(r->id, sizeof r->id, "%s", id);
   snprintf(r->actor, sizeof r->actor, "%s", actor);
   r->events = cJSON_CreateArray();
   r->envelopes = cJSON_CreateArray();
   pthread_mutex_unlock(&lock);
   return 0;
}
int db1_session_ir_running_history_load(const char *session, const char *actor, const char *turn,
                                        char **out)
{
   (void)session;
   (void)actor;
   (void)turn;
   *out = NULL;
   assert(!"this producer fixture does not request a native startup snapshot");
   return -1;
}
static int verified_appends, delegate_experiences, job_matches = 1, workspace_held = 1;
static cJSON *experience_source;
int db1_agent_job_get(int id, db1_agent_job_t *out)
{
   assert(id == job);
   memset(out, 0, sizeof *out);
   snprintf(out->role, sizeof out->role, "%s", job_matches ? "review" : "other");
   snprintf(out->agent_name, sizeof out->agent_name, "selected-agent");
   return 0;
}
void db1_agent_job_free(db1_agent_job_t *row)
{
   (void)row;
}
int workspace_turn_source_identity(const char *cwd, int *fd, char digest[65])
{
   assert(cwd && cwd[0]);
   *fd = -1;
   snprintf(digest, 65, "%064d", 1);
   return workspace_held ? 0 : -1;
}
int server_learning_project_from_cwd(const char *cwd, char *out, size_t capacity)
{
   assert(cwd && cwd[0]);
   snprintf(out, capacity, "registered-project");
   return 0;
}
int db1_session_ir_append_verified(const char *session, const char *event, const char *turn,
                                   const char *actor, const char *payload, char sequence[32])
{
   cJSON *json = cJSON_Parse(payload);
   assert(json && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(
                      cJSON_GetObjectItemCaseSensitive(json, "payload"), "learning_binding")));
   cJSON_Delete(json);
   verified_appends++;
   return db1_session_ir_append(session, event, turn, actor, payload, sequence);
}
char *db1_delegate_execution_terminal_lookup(const char *actor, const char *source_wire)
{
   cJSON *source = cJSON_Parse(source_wire);
   assert(source);
   const char *parent_id =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "session_id"));
   int sequence = atoi(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "sequence")));
   struct row *parent = find(parent_id);
   assert(parent && !strcmp(parent->actor, actor));
   const cJSON *parent_event = cJSON_GetArrayItem(parent->events, sequence - 1);
   const cJSON *binding = cJSON_GetObjectItemCaseSensitive(parent_event, "payload");
   const char *session =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(binding, "session_id"));
   const char *turn = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(binding, "turn_id"));
   struct row *child = find(session);
   assert(child && !strcmp(child->actor, actor));
   char *wire = NULL;
   const cJSON *entry;
   cJSON_ArrayForEach(entry, child->envelopes)
   {
      const cJSON *event = cJSON_GetObjectItemCaseSensitive(entry, "event");
      const cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "kind"));
      const char *entry_turn =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "turn_id"));
      const char *status =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "status"));
      if (kind && !strcmp(kind, "execution") && entry_turn && !strcmp(entry_turn, turn) && status &&
          strcmp(status, "running"))
      {
         cJSON *record = cJSON_Duplicate(entry, 1);
         cJSON_AddNumberToObject(record, "version", 1);
         cJSON_AddStringToObject(record, "session_id", session);
         wire = cJSON_PrintUnformatted(record);
         cJSON_Delete(record);
         break;
      }
   }
   cJSON_Delete(source);
   return wire;
}

int db1_delegate_execution_experience_record(const char *principal, const char *wire)
{
   cJSON *source = cJSON_Parse(wire);
   assert(source);
   const char *session =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "session_id"));
   int sequence = atoi(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "sequence")));
   assert(!strcmp(principal, "alice"));
   struct row *row = find(session);
   assert(row && !strcmp(row->actor, principal));
   const cJSON *event = cJSON_GetArrayItem(row->events, sequence - 1);
   const cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "status")),
                  "running"));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(payload, "learning_binding")));
   if (!experience_source)
   {
      experience_source = cJSON_Duplicate(source, 1);
      delegate_experiences++;
   }
   else
      assert(cJSON_Compare(experience_source, source, 1));
   cJSON_Delete(source);
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
   cJSON *envelope = cJSON_CreateObject();
   cJSON_AddStringToObject(envelope, "sequence", sequence);
   cJSON_AddStringToObject(envelope, "event_id", id);
   cJSON_AddStringToObject(envelope, "turn_id", turn);
   cJSON_AddStringToObject(envelope, "actor", actor);
   cJSON_AddItemToObject(envelope, "event", cJSON_Duplicate(event, 1));
   cJSON_AddItemToArray(r->envelopes, envelope);
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
   const char *source =
       (history_fault == 1 || history_fault == 3)
           ? "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_"
             "id\":\"pending-write\",\"name\":\"write_file\",\"input\":{\"ratio\":0."
             "12345678901234567890123456789}}]}]"
       : history_fault == 2 ? "[{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_"
                              "id\":\"orphan\",\"content\":\"unmatched\"}]}]"
                            : r->history;
   *out = source ? strdup(source) : NULL;
   if (history_fault == 3 && *out)
   {
      cJSON *compatibility = cJSON_Parse(*out);
      assert(compatibility && aimee_ir_history_repair(compatibility) == 1);
      free(*out);
      *out = cJSON_PrintUnformatted(compatibility);
      cJSON_Delete(compatibility);
   }
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
static void unresolved_history_requires_reconciliation(void)
{
   aimee_ir_session_set_sink(NULL, NULL);
   attest("parent", "alice");
   job = 10; /* Reuse the worker's existing private child and valid source. */
   server_session_log_t *scope = NULL;
   assert(!server_session_log_begin("review", "reconcile", &scope));
   char session[128], actor[VAULT_PRINCIPAL_MAX];
   assert(!server_session_log_identity(session, sizeof session, actor, sizeof actor));
   struct row *row = find(session);
   assert(row && row->history);
   char *before = strdup(row->history);
   assert(before);
   for (history_fault = 1; history_fault <= 3; history_fault++)
   {
      cJSON *history = NULL;
      assert(server_session_log_history_load(&history) == -1 && !history);
      assert(!strcmp(row->history, before)); /* Never checkpoint a fabricated repair. */
   }
   history_fault = 0;
   cJSON *history = NULL;
   assert(!server_session_log_history_load(&history) && cJSON_GetArraySize(history) == 1);
   cJSON_Delete(history);
   free(before);
   model_reply();
   assert(!server_session_log_finish(scope, 1, NULL));
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

static void source_bound_selected_delegate(void)
{
   attest("learning-parent", "alice");
   job = 901;
   run_cmd_set_cwd("/approved/workspace");
   server_session_log_t *scope = NULL;
   assert(!server_session_log_begin_selected("review", "actual selected attempt", "selected-agent",
                                             "actual-provider", "actual-model", &scope));
   assert(verified_appends == 1 && delegate_experiences == 0);
   server_session_log_t *nested = NULL;
   assert(!server_session_log_begin_selected("review", "same-role synchronous nested call",
                                             "selected-agent", "actual-provider", "actual-model",
                                             &nested));
   model_reply();
   assert(!server_session_log_finish(nested, 1, NULL));
   assert(verified_appends == 1 && delegate_experiences == 0);
   struct row *parent = find("learning-parent");
   assert(parent);
   cJSON *running = cJSON_Duplicate(cJSON_GetArrayItem(parent->events, 0), 1);
   const cJSON *binding = cJSON_GetObjectItemCaseSensitive(
       cJSON_GetObjectItemCaseSensitive(running, "payload"), "learning_binding");
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(binding, "job_id")), "901"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(binding, "provider")),
                  "actual-provider"));
   cJSON *invalid_binding = cJSON_Duplicate(binding, 1);
   aimee_session_event_t typed_child = {.kind = AIMEE_SESSION_CHILD_EXECUTION,
                                        .child_session = "delegate-exact",
                                        .child_turn = "exact-turn",
                                        .child_role = "review",
                                        .execution_status = "running",
                                        .child_learning_binding = invalid_binding};
   cJSON *typed_json = aimee_ir_session_event_json(&typed_child);
   assert(typed_json);
   cJSON_Delete(typed_json);
   cJSON_ReplaceItemInObjectCaseSensitive(invalid_binding, "job_id",
                                          cJSON_CreateString("2147483648"));
   assert(!aimee_ir_session_event_json(&typed_child));
   cJSON_ReplaceItemInObjectCaseSensitive(invalid_binding, "job_id", cJSON_CreateString("901"));
   cJSON_AddStringToObject(invalid_binding, "extra", "forbidden");
   assert(!aimee_ir_session_event_json(&typed_child));
   cJSON_Delete(invalid_binding);
   model_reply();
   assert(!server_session_log_finish(scope, 1, NULL));
   assert(verified_appends == 2 && delegate_experiences == 1);
   const cJSON *source = experience_source;
   server_learning_observe_child(
       "learning-parent", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "turn_id")),
       "1", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "event_id")), "alice",
       running);
   assert(delegate_experiences == 1); /* Retry uses exact running source, never terminal ref. */
   cJSON_Delete(running);
   job_matches = 0;
   assert(!server_session_log_begin_selected("nested-role", "nested model call", "selected-agent",
                                             "actual-provider", "actual-model", &scope));
   model_reply();
   assert(!server_session_log_finish(scope, 1, NULL));
   assert(verified_appends == 2 && delegate_experiences == 1);
   job_matches = 1;
   workspace_held = 0;
   assert(!server_session_log_begin_selected("review", "unheld workspace", "selected-agent",
                                             "actual-provider", "actual-model", &scope));
   model_reply();
   assert(!server_session_log_finish(scope, 1, NULL));
   assert(verified_appends == 2 && delegate_experiences == 1);
   workspace_held = 1;
   /* Child terminal is durable even when parent terminal publication is lost.
    * The pre-execution running reference alone must recover the projection. */
   cJSON_Delete(experience_source);
   experience_source = NULL;
   delegate_experiences = 0;
   attest("crash-learning-parent", "alice");
   job = 902;
   assert(!server_session_log_begin_selected("review", "terminal publication interrupted",
                                             "selected-agent", "actual-provider", "actual-model",
                                             &scope));
   parent = find("crash-learning-parent");
   running = cJSON_Duplicate(cJSON_GetArrayItem(parent->events, 0), 1);
   cJSON *envelope = cJSON_GetArrayItem(parent->envelopes, 0);
   char turn[128], id[192];
   snprintf(turn, sizeof turn, "%s",
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "turn_id")));
   snprintf(id, sizeof id, "%s",
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "event_id")));
   model_reply();
   reject_reference = 1;
   assert(server_session_log_finish(scope, 1, NULL) < 0);
   reject_reference = 0;
   assert(delegate_experiences == 0 && cJSON_GetArraySize(parent->events) == 1);
   server_learning_observe_child("crash-learning-parent", turn, "1", id, "alice", running);
   assert(delegate_experiences == 1);
   server_learning_observe_child("crash-learning-parent", turn, "1", id, "alice", running);
   assert(delegate_experiences == 1);
   cJSON_Delete(running);
   run_cmd_set_cwd(NULL);
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
   unresolved_history_requires_reconciliation();
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
   source_bound_selected_delegate();
   cJSON_Delete(experience_source);
   for (int i = 0; i < count; i++)
   {
      cJSON_Delete(rows[i].events);
      cJSON_Delete(rows[i].envelopes);
      free(rows[i].history);
   }
   puts("log-only owners: copied attestation, private concurrent children, durable reload and "
        "terminal failures passed");
   return 0;
}
