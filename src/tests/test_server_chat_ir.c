#include "server_compute_impl.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *db1_delegate_execution_terminal_lookup(const char *principal, const char *source)
{
   (void)principal;
   (void)source;
   assert(!"unexpected child terminal lookup in primary chat fixture");
   return NULL;
}

/* Primary chat never owns a child job binding. */
int db1_delegate_execution_experience_record(const char *principal, const char *source)
{
   (void)principal;
   (void)source;
   assert(!"unexpected delegate projection in primary chat fixture");
   return -1;
}

static cJSON *log_entries;
static int writes, fail_store, disconnected, cancelled, delivered_sequence;
static server_session_capture_t *page_writer;
static char owner[64] = "alice";
void test_learning_observations_reset(void);
int test_learning_observations(void);
void test_learning_assert_source(const cJSON *record)
{
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "session_id")),
                  "session-one"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "actor")), owner));
   int sequence = atoi(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "sequence")));
   assert(sequence > 0 && sequence <= cJSON_GetArraySize(log_entries));
   assert(cJSON_Compare(cJSON_GetArrayItem(log_entries, sequence - 1),
                        cJSON_GetObjectItemCaseSensitive(record, "event"), 1));
}

int presence_turn_host_own(const char *sid, const char *turn)
{
   (void)sid;
   (void)turn;
   return 1;
}
int agent_request_cancelled(void)
{
   return cancelled;
}
int db1_server_session_get(const char *id, db1_server_session_t *row)
{
   assert(!strcmp(id, "session-one"));
   memset(row, 0, sizeof(*row));
   snprintf(row->principal, sizeof(row->principal), "%s", owner);
   return 0;
}
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   (void)id;
   (void)client;
   (void)principal;
   assert(0);
   return -1;
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
int db1_session_ir_append(const char *sid, const char *eid, const char *turn, const char *actor,
                          const char *payload, char sequence[32])
{
   assert(!strcmp(sid, "session-one") && !strcmp(actor, owner));
   assert(eid[0] && turn[0]);
   if (fail_store)
      return -1;
   cJSON *event = cJSON_Parse(payload);
   assert(event);
   cJSON_AddItemToArray(log_entries, event);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(log_entries));
   return 0;
}
/* This fixture has no private verifier owner. Privileged proof appends must
 * remain unreachable; ordinary event capture retains its existing test path. */
int db1_session_ir_append_verified(const char *session, const char *event, const char *turn,
                                   const char *actor, const char *payload, char sequence[32])
{
   (void)session;
   (void)event;
   (void)turn;
   (void)actor;
   (void)payload;
   (void)sequence;
   assert(!"unexpected verifier proof in ordinary capture fixture");
   return -1;
}
char *db1_session_ir_page(const char *sid, const char *after, int limit, const char *principal)
{
   assert(!strcmp(sid, "session-one") && !strcmp(principal, owner) && limit == 128);
   cJSON *events = cJSON_CreateArray();
   if (page_writer)
   {
      /* Another retained callback can commit while the subscriber fetches a
       * page. Its envelope may be delivered beyond the sampled target. */
      aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "concurrent tail"};
      aimee_message_t message = {.role = "assistant", .blocks = &text, .n_blocks = 1};
      aimee_session_event_t event = {
          .kind = AIMEE_SESSION_MESSAGE, .message_id = "late", .message = &message};
      int rc = server_session_capture_sink(&event, page_writer);
      assert(!rc);
      page_writer = NULL;
   }
   int start = atoi(after);
   for (int i = start; i < cJSON_GetArraySize(log_entries) && i < start + limit; i++)
   {
      cJSON *envelope = cJSON_CreateObject();
      char sequence[32];
      snprintf(sequence, sizeof sequence, "%d", i + 1);
      cJSON_AddNumberToObject(envelope, "version", 1);
      cJSON_AddStringToObject(envelope, "session_id", sid);
      cJSON_AddStringToObject(envelope, "sequence", sequence);
      cJSON_AddStringToObject(envelope, "event_id", sequence);
      cJSON_AddStringToObject(envelope, "turn_id", "checkpoint");
      cJSON_AddStringToObject(envelope, "actor", owner);
      cJSON_AddItemToObject(envelope, "event",
                            cJSON_Duplicate(cJSON_GetArrayItem(log_entries, i), 1));
      cJSON_AddItemToArray(events, envelope);
   }
   char *json = cJSON_PrintUnformatted(events);
   cJSON_Delete(events);
   return json;
}
int write_all(int fd, const char *text, size_t length)
{
   (void)fd;
   if (disconnected)
      return -1;
   if (length == 1 && text[0] == '\n')
      return 0;
   cJSON *envelope = cJSON_ParseWithLength(text, length);
   assert(envelope);
   int committed = cJSON_GetArraySize(log_entries);
   assert(committed > 0);
   const char *sequence = cJSON_GetStringValue(cJSON_GetObjectItem(envelope, "sequence"));
   int delivered = sequence ? atoi(sequence) : 0;
   assert(delivered > delivered_sequence && delivered <= committed);
   delivered_sequence = delivered;
   assert(cJSON_Compare(cJSON_GetObjectItem(envelope, "event"),
                        cJSON_GetArrayItem(log_entries, delivered - 1), 1));
   cJSON_Delete(envelope);
   writes++;
   return 0;
}
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static compute_ctx_t context(void)
{
   compute_ctx_t c = {0};
   c.presence_locked = 1;
   c.ir_delivery = 1;
   c.conn_alive = 1;
   c.write_mutex = &mutex;
   snprintf(c.vault_principal, sizeof(c.vault_principal), "alice");
   return c;
}
static void cleanup(compute_ctx_t *c)
{
   server_chat_ir_close(c);
   cJSON_Delete(log_entries);
   log_entries = cJSON_CreateArray();
   writes = fail_store = disconnected = cancelled = delivered_sequence = 0;
   page_writer = NULL;
   test_learning_observations_reset();
}
static void response(void)
{
   aimee_block_t use = {.type = AIMEE_BLK_TOOL_USE, .tool_id = "opaque:one", .tool_name = "Bash"};
   aimee_message_t m = {.role = "assistant", .blocks = &use, .n_blocks = 1};
   aimee_session_event_t e = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "message-one", .message = &m};
   assert(!aimee_ir_session_emit(&e));
   assert(!aimee_ir_session_emit_tool_result("opaque:one", "done", 0));
   const cJSON *u = cJSON_GetArrayItem(
       cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 2), "payload"),
                           "blocks"),
       0);
   const cJSON *r = cJSON_GetArrayItem(
       cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 3), "payload"),
                           "blocks"),
       0);
   const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(u, "tool_id"));
   assert(!strcmp(uid, "opaque:one"));
   assert(!strcmp(uid, cJSON_GetStringValue(cJSON_GetObjectItem(r, "tool_id"))));
}
static char legacy_text[4096], legacy_thinking[4096];
static int legacy_calls;
static int legacy_project(compute_ctx_t *c, const char *event, const char *key, const char *value)
{
   assert(c->ir_native_owned && cJSON_GetArraySize(log_entries) > 2);
   if (!strcmp(event, "text") || !strcmp(event, "thinking"))
   {
      assert(key && !strcmp(key, "content") && value);
      char *output = !strcmp(event, "text") ? legacy_text : legacy_thinking;
      assert(strlen(output) + strlen(value) < sizeof legacy_text);
      strcat(output, value);
      legacy_calls++;
   }
   return c->conn_alive ? 0 : -1;
}
static void legacy_uses_committed_originals(void)
{
   compute_ctx_t c = context();
   c.ir_delivery = 0;
   assert(!server_chat_ir_begin(&c, "session-one", "canonical compatibility"));
   c.ir_native_owned = 1;
   c.ir_legacy_emit = legacy_project;
   legacy_text[0] = legacy_thinking[0] = 0;
   legacy_calls = 0;
   server_session_capture_t *worker = NULL;
   assert(!server_session_capture_retain_current("session-one", "alice", &worker) && worker);
   aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_DELTA,
                          .kind = AIMEE_BLK_TEXT,
                          .block_id = 0,
                          .text_delta = "Original "};
   aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA,
                                  .role = "assistant",
                                  .message_id = "native:original",
                                  .delta = &delta};
   assert(!server_session_capture_sink(&event, worker));
   assert(!legacy_calls && !server_chat_ir_progress(&c));
   assert(legacy_calls == 1 && !strcmp(legacy_text, "Original "));
   /* A disconnected subscriber must not stop canonical ring/poll projection. */
   c.conn_alive = 0;
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "Original source"},
                             {.type = AIMEE_BLK_THINKING, .text = "Original reasoning"}};
   aimee_message_t message = {.role = "assistant", .blocks = blocks, .n_blocks = 2};
   event = (aimee_session_event_t){
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "native:original", .message = &message};
   assert(!server_session_capture_sink(&event, worker));
   assert(!server_chat_ir_progress(&c));
   assert(!strcmp(legacy_text, "Original source"));
   assert(!strcmp(legacy_thinking, "Original reasoning") && legacy_calls == 3);
   /* A repeated final snapshot never duplicates already rendered source. */
   assert(!server_session_capture_sink(&event, worker));
   assert(!server_chat_ir_progress(&c) && legacy_calls == 3);
   assert(!server_chat_ir_progress(&c) && legacy_calls == 3);
   /* A source inconsistency is observable and sticky, never a silent rewrite. */
   blocks[0].text = "conflicting SDK rewrite";
   assert(!server_session_capture_sink(&event, worker));
   assert(server_chat_ir_progress(&c) < 0 && c.ir_failed);
   assert(!strcmp(legacy_text, "Original source"));
   server_session_capture_release(worker);
   cleanup(&c);
}

static void exact_tool_identity(void)
{
   compute_ctx_t c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "native identity"));
   char id[1025];
   memset(id, 'x', sizeof id - 1);
   id[sizeof id - 1] = 0;
   aimee_block_t use = {.type = AIMEE_BLK_TOOL_USE, .tool_id = id, .tool_name = "read_file"};
   aimee_message_t message = {.role = "assistant", .blocks = &use, .n_blocks = 1};
   for (int cycle = 0; cycle < 2; cycle++)
   {
      aimee_session_event_t event = {.kind = AIMEE_SESSION_MESSAGE,
                                     .message_id = cycle ? "second" : "first",
                                     .message = &message};
      assert(!aimee_ir_session_emit(&event));
      assert(!aimee_ir_session_emit_tool_result(id, cycle ? "SECOND" : "FIRST", 0));
      for (int i = 2 + cycle * 2; i < 4 + cycle * 2; i++)
      {
         const cJSON *body = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, i), "payload");
         const cJSON *block = cJSON_GetArrayItem(cJSON_GetObjectItem(body, "blocks"), 0);
         assert(!strcmp(id, cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_id"))));
      }
   }
   aimee_delta_t start = {.type = AIMEE_DELTA_BLOCK_START,
                          .kind = AIMEE_BLK_TOOL_USE,
                          .tool_id = id,
                          .tool_name = "read_file"};
   aimee_session_event_t delta = {
       .kind = AIMEE_SESSION_DELTA, .role = "assistant", .message_id = "streamed", .delta = &start};
   assert(!aimee_ir_session_emit(&delta));
   const cJSON *body = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 6), "payload");
   assert(!strcmp(id, cJSON_GetStringValue(cJSON_GetObjectItem(body, "tool_id"))));
   assert(!strcmp(use.tool_id, id) && !strcmp(start.tool_id, id));
   assert(!server_chat_ir_finish(&c, NULL));
   cleanup(&c);
}

static int failed_control_record(const char *session, const char *turn, const char *principal,
                                 const aimee_session_event_t *event)
{
   assert(!strcmp(session, "session-one") && turn[0] && !strcmp(principal, "alice"));
   assert(event->kind == AIMEE_SESSION_CONTROL_REQUEST);
   return -1;
}
static void worker_capture_failure_is_sticky(void)
{
   compute_ctx_t c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "question capture"));
   cJSON *request = cJSON_Parse("{\"title\":\"Question\"}");
   assert(server_control_register_recorded(c.ir_controls, "question", "permission", request,
                                           failed_control_record) < 0);
   cJSON_Delete(request);
   assert(server_control_scope_failed(c.ir_controls));
   aimee_response_t response = {.role = "assistant"};
   assert(aimee_ir_session_emit_response(&response, "after-worker-failure") < 0);
   assert(c.ir_failed && writes == 2 && cJSON_GetArraySize(log_entries) == 2);
   assert(server_chat_ir_finish(&c, NULL) < 0);
   cleanup(&c);
}

static void *fail_capture_worker(void *arg)
{
   server_session_capture_t *capture = arg;
   server_session_capture_fail(capture);
   server_session_capture_release(capture);
   return NULL;
}
static void *write_retained_tail(void *arg)
{
   server_session_capture_t *capture = arg;
   for (int i = 0; i < 131; i++)
   {
      char id[32];
      snprintf(id, sizeof id, "worker-%d", i);
      aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "original provider content"};
      aimee_message_t message = {.role = "assistant", .blocks = &text, .n_blocks = 1};
      aimee_session_event_t event = {
          .kind = AIMEE_SESSION_MESSAGE, .message_id = id, .message = &message};
      int written = server_session_capture_sink(&event, capture);
      assert(!written);
   }
   server_session_capture_release(capture);
   return NULL;
}
static void retained_worker_progress_uses_durable_pages(void)
{
   compute_ctx_t c = context();
   int started = server_chat_ir_begin(&c, "session-one", "retained native progress");
   assert(!started && writes == 2);
   server_session_capture_t *capture = NULL;
   int retained = server_session_capture_retain_current("session-one", "alice", &capture);
   assert(!retained && capture);
   pthread_t worker;
   int spawned = pthread_create(&worker, NULL, write_retained_tail, capture);
   assert(!spawned);
   int joined = pthread_join(worker, NULL);
   assert(!joined && writes == 2 && cJSON_GetArraySize(log_entries) == 133);
   page_writer = c.ir_capture;
   int progress = server_chat_ir_progress(&c);
   assert(!progress && writes == 134 && !strcmp(c.ir_sequence, "134"));
   progress = server_chat_ir_progress(&c);
   assert(!progress && writes == 134); /* No duplicate or backward cursor. */
   int finished = server_chat_ir_finish(&c, NULL);
   assert(!finished && writes == 135 && !strcmp(c.ir_sequence, "135"));
   progress = server_chat_ir_progress(&c);
   assert(!progress && writes == 135); /* Sealed writer still exposes its tail. */
   cleanup(&c);
}

static void retained_worker_failure_refuses_completion(void)
{
   for (int model = 0; model < 2; model++)
   {
      compute_ctx_t c = context();
      int started = server_chat_ir_begin(&c, "session-one", "retained worker");
      assert(!started);
      if (model)
         response();
      int before = cJSON_GetArraySize(log_entries);
      server_session_capture_t *capture = NULL;
      int retained = server_session_capture_retain_current("session-one", "alice", &capture);
      assert(!retained && capture == c.ir_capture);
      pthread_t worker;
      int spawned = pthread_create(&worker, NULL, fail_capture_worker, capture);
      assert(!spawned);
      int joined = pthread_join(worker, NULL);
      assert(!joined && !aimee_ir_session_capture_failed());
      int completed = server_chat_ir_finish(&c, NULL);
      assert(completed < 0 && c.ir_failed && cJSON_GetArraySize(log_entries) == before);
      assert(!test_learning_observations());
      cleanup(&c);
   }
}

int main(void)
{
   log_entries = cJSON_CreateArray();
   worker_capture_failure_is_sticky();
   retained_worker_failure_refuses_completion();
   retained_worker_progress_uses_durable_pages();
   legacy_uses_committed_originals();
   exact_tool_identity();
   compute_ctx_t legacy = context();
   legacy.ir_delivery = 0;
   assert(!server_chat_ir_begin(&legacy, "session-one", "legacy subscriber"));
   response();
   assert(!server_chat_ir_finish(&legacy, NULL));
   assert(cJSON_GetArraySize(log_entries) == 5 && writes == 0);
   cleanup(&legacy);

   for (int committed = 0; committed < 2; committed++)
   {
      compute_ctx_t history = context();
      int opened = server_chat_ir_begin(&history, "session-one", NULL);
      assert(!opened && cJSON_GetArraySize(log_entries) == 1);
      history.ir_history_checkpointed = committed;
      int finished = server_chat_ir_finish(&history, NULL);
      assert(!finished && cJSON_GetArraySize(log_entries) == 2);
      const cJSON *terminal = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 1), "payload");
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(terminal, "status")),
                     committed ? "completed" : "failed"));
      cleanup(&history);
   }

   compute_ctx_t checkpoint_context = context();
   assert(!server_chat_ir_begin(&checkpoint_context, "session-one", "hello"));
   cJSON_AddItemToArray(log_entries, cJSON_Parse("{\"kind\":\"history\",\"payload\":{\"scope\":"
                                                 "\"primary\",\"phase\":\"commit\",\"count\":0}}"));
   assert(!server_chat_ir_finish(&checkpoint_context, NULL));
   assert(writes == 4 && !checkpoint_context.ir_failed);
   cleanup(&checkpoint_context);

   compute_ctx_t c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   assert(writes == 2);
   disconnected = 1;
   response();
   assert(!server_chat_ir_finish(&c, NULL));
   assert(test_learning_observations() == 1);
   assert(!c.conn_alive && writes == 2 && cJSON_GetArraySize(log_entries) == 5);
   const cJSON *last = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 4), "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last, "status")), "completed"));
   assert(!server_chat_ir_finish(&c, NULL) && cJSON_GetArraySize(log_entries) == 5);
   cleanup(&c);

   c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   fail_store = 1;
   aimee_response_t ir = {.role = "assistant"};
   assert(aimee_ir_session_emit_response(&ir, "failed") < 0);
   assert(c.ir_failed && writes == 2 && cJSON_GetArraySize(log_entries) == 2);
   cleanup(&c);

   c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   cancelled = 1;
   assert(!server_chat_ir_finish(&c, NULL));
   last = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 2), "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last, "status")), "cancelled"));
   cleanup(&c);

   c = context();
   assert(!server_chat_ir_begin(&c, "session-one", "hello"));
   assert(!server_chat_ir_finish(&c, NULL));
   last = cJSON_GetObjectItem(cJSON_GetArrayItem(log_entries, 2), "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last, "status")), "failed"));
   cleanup(&c);

   c = context();
   snprintf(owner, sizeof(owner), "bob");
   assert(server_chat_ir_begin(&c, "session-one", "hello") < 0 && !cJSON_GetArraySize(log_entries));
   snprintf(owner, sizeof(owner), "alice");
   c.presence_locked = 0;
   assert(server_chat_ir_begin(&c, "session-one", "hello") < 0);
   cJSON_Delete(log_entries);
   puts("server canonical capture tests passed");
   return 0;
}
