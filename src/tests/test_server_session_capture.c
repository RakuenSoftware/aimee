#include "server_session_capture.h"
#include "server_session_control.h"
#include "server_learning_outcome.h"
#include "db1_client/primary_sessions.h"
#include "cJSON.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t storage = PTHREAD_MUTEX_INITIALIZER;
static cJSON *rows;
static int reject_append, observations;
static int history_status;
static const char *history_wire;
int db1_session_ir_running_history_load(const char *session, const char *actor, const char *turn,
                                        char **out)
{
   assert(!strcmp(session, "private-child") && !strcmp(actor, "alice") && !strcmp(turn, "turn"));
   *out = history_status ? NULL : strdup(history_wire);
   return history_status;
}
int db1_session_ir_append(const char *session, const char *id, const char *turn,
                          const char *principal, const char *wire, char sequence[32])
{
   assert(!strcmp(session, "private-child") && !strcmp(principal, "alice"));
   assert(!strncmp(id, turn, strlen(turn)) && id[strlen(turn)] == ':');
   pthread_mutex_lock(&storage);
   if (reject_append)
   {
      pthread_mutex_unlock(&storage);
      return -1;
   }
   cJSON *payload = aimee_ir_json_parse_frame(wire, strlen(wire));
   assert(payload);
   cJSON *row = cJSON_CreateObject();
   assert(row);
   cJSON_AddStringToObject(row, "id", id);
   cJSON_AddItemToObject(row, "event", payload);
   const cJSON *old;
   cJSON_ArrayForEach(old, rows)
   {
      assert(strcmp(id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(old, "id"))));
   }
   cJSON_AddItemToArray(rows, row);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(rows));
   pthread_mutex_unlock(&storage);
   return 0;
}
void server_learning_observe_terminal(const char *session, const char *turn, const char *sequence,
                                      const char *id, const char *principal, const cJSON *event)
{
   assert(!strcmp(session, "private-child") && !strcmp(principal, "alice") && turn[0]);
   pthread_mutex_lock(&storage);
   const cJSON *last = cJSON_GetArrayItem(rows, cJSON_GetArraySize(rows) - 1);
   assert(atoi(sequence) == cJSON_GetArraySize(rows));
   assert(!strcmp(id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(last, "id"))));
   assert(cJSON_Compare(event, cJSON_GetObjectItemCaseSensitive(last, "event"), 1));
   observations++;
   pthread_mutex_unlock(&storage);
}
static void reset(void)
{
   assert(!server_session_capture_current());
   cJSON_Delete(rows);
   rows = cJSON_CreateArray();
   assert(rows);
   reject_append = observations = 0;
}
typedef struct
{
   server_session_capture_t *owner;
   int index;
} worker_t;
static void *write_message(void *arg)
{
   worker_t *worker = arg;
   assert(!server_session_capture_bind(worker->owner));
   char id[32], text[32];
   snprintf(id, sizeof id, "worker-%d", worker->index);
   snprintf(text, sizeof text, "answer %d", worker->index);
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = text};
   aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = id, .message = &message};
   char *envelope = NULL, sequence[32];
   int written = server_session_capture_append(worker->owner, &event, &envelope, sequence);
   assert(!written && envelope && sequence[0]);
   cJSON *view = aimee_ir_json_parse_frame(envelope, strlen(envelope));
   assert(view);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(view, "session_id")),
                  "private-child"));
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(view, "sequence")), sequence));
   cJSON_Delete(view);
   free(envelope);
   assert(server_session_capture_bind(NULL) == worker->owner);
   server_session_capture_release(worker->owner);
   return NULL;
}
static void test_concurrent_owner(void)
{
   reset();
   char session[] = "private-child", turn[] = "turn", actor[] = "alice", source[] = "parent";
   server_session_capture_t *owner = server_session_capture_open(session, turn, actor, source);
   assert(owner && !server_session_capture_bind(owner));
   memset(session, 'x', strlen(session));
   memset(turn, 'x', strlen(turn));
   memset(actor, 'x', strlen(actor));
   memset(source, 'x', strlen(source));
   assert(!server_session_capture_session(NULL));
   assert(!strcmp(server_session_capture_session(owner), "private-child"));
   server_session_capture_t *lease = NULL;
   int wrong_actor = server_session_capture_retain_current("parent", "mallory", &lease);
   assert(wrong_actor < 0 && !lease);
   int wrong_source = server_session_capture_retain_current("private-child", "alice", &lease);
   assert(wrong_source < 0 && !lease);
   const char *arguments = "{\"amount\":123.123456789012345678901234567890}";
   cJSON *input = aimee_ir_json_parse_frame(arguments, strlen(arguments));
   assert(input);
   aimee_block_t tool = {.type = AIMEE_BLK_TOOL_USE,
                         .tool_id = "native-call:original",
                         .tool_name = "exec_command",
                         .tool_input = input};
   aimee_message_t assistant = {.role = "assistant", .blocks = &tool, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "assistant", .message = &assistant};
   int written = server_session_capture_append(owner, &event, NULL, NULL);
   assert(!written);
   aimee_ir_session_set_sink(server_session_capture_sink, owner);
   int result_written =
       aimee_ir_session_emit_tool_result("native-call:original", "original result", 0);
   assert(!result_written);
   aimee_ir_session_set_sink(NULL, NULL);
   const cJSON *payload = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "event");
   char *wire = cJSON_PrintUnformatted(payload);
   assert(wire && strstr(wire, "123.123456789012345678901234567890"));
   assert(strstr(wire, "native-call:original") && strstr(wire, "exec_command") &&
          strstr(wire, "turn:assistant"));
   free(wire);
   pthread_t threads[8];
   worker_t workers[8];
   for (int i = 0; i < 8; i++)
   {
      int retained = server_session_capture_retain_current("parent", "alice", &workers[i].owner);
      assert(!retained && workers[i].owner == owner);
      workers[i].index = i;
      int started = pthread_create(&threads[i], NULL, write_message, &workers[i]);
      assert(!started);
   }
   for (int i = 0; i < 8; i++)
   {
      int joined = pthread_join(threads[i], NULL);
      assert(!joined);
   }
   char cursor[32];
   int cursor_read = server_session_capture_cursor(owner, cursor);
   assert(!cursor_read && !strcmp(cursor, "10"));
   server_session_capture_counts_t counts;
   int counted = server_session_capture_counts(owner, &counts);
   assert(!counted && counts.counter == 10 && counts.model_events == 9 &&
          counts.model_completions == 9);
   aimee_session_event_t terminal = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = "completed"};
   int finished = server_session_capture_sink(&terminal, owner);
   assert(!finished && observations == 1 && !server_session_capture_accepting(owner));
   cursor_read = server_session_capture_cursor(owner, cursor);
   assert(!cursor_read && !strcmp(cursor, "11"));
   int late = server_session_capture_sink(&event, owner);
   assert(late < 0 && cJSON_GetArraySize(rows) == 11 && !server_session_capture_failed(owner));
   cJSON_Delete(input);
   server_session_capture_close(owner);
}
static void *closed_writer(void *arg)
{
   server_session_capture_t *owner = arg;
   assert(!server_session_capture_bind(owner));
   server_session_capture_t *lease = NULL;
   int retained = server_session_capture_retain_current("parent", "alice", &lease);
   assert(retained < 0 && !lease);
   aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   int written = server_session_capture_sink(&event, owner);
   assert(written < 0);
   server_session_capture_bind(NULL);
   server_session_capture_release(owner);
   return NULL;
}
static void test_close_before_callback(void)
{
   reset();
   server_session_capture_t *owner =
       server_session_capture_open("private-child", "late", "alice", "parent");
   assert(owner && !server_session_capture_bind(owner));
   server_session_capture_t *lease = NULL;
   int retained = server_session_capture_retain_current("parent", "alice", &lease);
   assert(!retained && lease == owner);
   server_session_capture_close(owner);
   pthread_t worker;
   int started = pthread_create(&worker, NULL, closed_writer, lease);
   assert(!started);
   int joined = pthread_join(worker, NULL);
   assert(!joined && !cJSON_GetArraySize(rows));
}
static void test_failure_refuses_success(void)
{
   reset();
   server_session_capture_t *owner =
       server_session_capture_open("private-child", "failed", "alice", "parent");
   assert(owner && !server_session_capture_bind(owner));
   aimee_session_event_t started = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   reject_append = 1;
   int refused = server_session_capture_sink(&started, owner);
   assert(refused < 0 && server_session_capture_failed(owner));
   reject_append = 0;
   aimee_session_event_t completed = {.kind = AIMEE_SESSION_EXECUTION,
                                      .execution_status = "completed"};
   int no_success = server_session_capture_sink(&completed, owner);
   assert(no_success < 0 && !cJSON_GetArraySize(rows) && !observations);
   aimee_session_event_t failed = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "failed"};
   int recorded = server_session_capture_sink(&failed, owner);
   assert(!recorded && cJSON_GetArraySize(rows) == 1 && observations == 1 &&
          server_session_capture_failed(owner));
   server_session_capture_close(owner);
}
static void test_running_history_source(void)
{
   reset();
   server_session_capture_t *owner =
       server_session_capture_open("private-child", "turn", "alice", "parent");
   assert(owner);
   history_status = 0;
   history_wire =
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"source-call\","
       "\"name\":\"exec_command\",\"input\":{\"amount\":123.123456789012345678901234567890}}]}]";
   cJSON *source = NULL;
   int loaded = server_session_capture_history(owner, &source);
   char *wire = source ? cJSON_PrintUnformatted(source) : NULL;
   assert(!loaded && wire && strstr(wire, "source-call") && strstr(wire, "exec_command") &&
          strstr(wire, "123.123456789012345678901234567890"));
   free(wire);
   cJSON_Delete(source);
   assert(!server_session_capture_failed(owner));
   server_session_capture_close(owner);
   const char *invalid[] = {"{}", "[] trailing",
                            "[{\"role\":\"user\",\"role\":\"assistant\",\"blocks\":[]}]",
                            "[{\"role\":\"operator\",\"blocks\":[]}]",
                            "[{\"role\":\"user\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":"
                            "\"forged\",\"name\":\"exec_command\",\"input\":{}}]}]"};
   for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0] + 2; i++)
   {
      owner = server_session_capture_open("private-child", "turn", "alice", "parent");
      assert(owner);
      history_status = i < sizeof invalid / sizeof invalid[0]    ? 0
                       : i == sizeof invalid / sizeof invalid[0] ? 1
                                                                 : -1;
      history_wire = history_status ? NULL : invalid[i];
      source = NULL;
      loaded = server_session_capture_history(owner, &source);
      assert(loaded < 0 && !source && server_session_capture_failed(owner));
      history_status = 0;
      history_wire = "[]";
      loaded = server_session_capture_history(owner, &source);
      assert(loaded < 0 && !source); /* A later healthy read cannot clear failure. */
      server_session_capture_close(owner);
   }
}

typedef struct
{
   server_session_capture_t *owner;
   int result;
} claim_worker_t;
static void *claim_original_source(void *context)
{
   claim_worker_t *worker = context;
   worker->result = server_session_capture_claim_call(worker->owner, "1", 0);
   server_session_capture_release(worker->owner);
   return NULL;
}
static void test_host_turn_generation_and_source_claims(void)
{
   reset();
   server_session_capture_t *owner =
       server_session_capture_open("private-child", "turn", "alice", "parent");
   assert(owner && !server_session_capture_bind(owner));
   server_session_capture_t *duplicate =
       server_session_capture_open("private-child", "turn", "alice", "parent");
   assert(!duplicate);
   duplicate = server_session_capture_open("private-child", "turn", "mallory", "other");
   assert(!duplicate);
   unsigned long long first = 0, refused = 0, second = 0;
   int entered = server_session_capture_model_enter(owner, &first);
   assert(!entered && first);
   entered = server_session_capture_model_enter(owner, &refused);
   assert(entered < 0 && !refused);
   int left = server_session_capture_model_leave(owner, first + 1);
   assert(left < 0);
   entered = server_session_capture_model_enter(owner, &refused);
   assert(entered < 0 && !refused);
   left = server_session_capture_model_leave(owner, first);
   assert(!left);
   entered = server_session_capture_model_enter(owner, &second);
   assert(!entered && second > first);
   left = server_session_capture_model_leave(owner, first);
   assert(left < 0); /* A stale leave cannot release the newer generation. */
   entered = server_session_capture_model_enter(owner, &refused);
   assert(entered < 0 && !refused);
   left = server_session_capture_model_leave(owner, second);
   assert(!left);
   int claim = server_session_capture_claim_call(owner, "1", 0);
   assert(claim < 0); /* No committed original source exists yet. */
   cJSON *input = cJSON_CreateObject();
   aimee_block_t *calls = calloc(AIMEE_IR_MAX_BLOCKS, sizeof *calls);
   assert(input && calls);
   for (int i = 0; i < AIMEE_IR_MAX_BLOCKS; i++)
   {
      char id[32];
      snprintf(id, sizeof id, "original-%d", i);
      calls[i] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                 .tool_id = strdup(id),
                                 .tool_name = "read_file",
                                 .tool_input = input};
      assert(calls[i].tool_id);
   }
   aimee_message_t message = {
       .role = "assistant", .blocks = calls, .n_blocks = AIMEE_IR_MAX_BLOCKS};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "original", .message = &message};
   int written = server_session_capture_sink(&event, owner);
   assert(!written);
   const char *invalid[] = {"", "0", "01", " 1", "+1", "-1", "18446744073709551616", "2"};
   for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
   {
      claim = server_session_capture_claim_call(owner, invalid[i], 0);
      assert(claim < 0);
   }
   claim = server_session_capture_claim_call(owner, "1", -1);
   assert(claim < 0);
   claim = server_session_capture_claim_call(owner, "1", AIMEE_IR_MAX_BLOCKS);
   assert(claim < 0);
   pthread_t threads[8];
   claim_worker_t workers[8];
   for (int i = 0; i < 8; i++)
   {
      int retained = server_session_capture_retain_current("parent", "alice", &workers[i].owner);
      assert(!retained && workers[i].owner == owner);
      int spawned = pthread_create(&threads[i], NULL, claim_original_source, &workers[i]);
      assert(!spawned);
   }
   int admitted = 0;
   for (int i = 0; i < 8; i++)
   {
      int joined = pthread_join(threads[i], NULL);
      assert(!joined && (workers[i].result == 0 || workers[i].result == 1));
      admitted += workers[i].result == 0;
   }
   assert(admitted == 1);
   for (int i = 1; i < AIMEE_IR_MAX_BLOCKS; i++)
   {
      claim = server_session_capture_claim_call(owner, "1", i);
      assert(!claim);
   }
   message.n_blocks = 1;
   event.message_id = "next";
   written = server_session_capture_sink(&event, owner);
   assert(!written);
   claim = server_session_capture_claim_call(owner, "2", 0);
   assert(claim < 0); /* Bounded transient claims never become another journal. */
   claim = server_session_capture_claim_call(owner, "1", 0);
   assert(claim == 1);
   for (int i = 0; i < AIMEE_IR_MAX_BLOCKS; i++)
      free(calls[i].tool_id);
   free(calls);
   cJSON_Delete(input);
   entered = server_session_capture_model_enter(owner, &second);
   assert(!entered);
   server_session_capture_close(owner);
   duplicate = server_session_capture_open("private-child", "turn", "alice", "parent");
   assert(!duplicate); /* A closed writer's retained generation still owns its identity. */
   entered = server_session_capture_model_enter(owner, &refused);
   assert(entered < 0 && !refused);
   claim = server_session_capture_claim_call(owner, "1", 0);
   assert(claim < 0);
   left = server_session_capture_model_leave(owner, second);
   assert(!left); /* The generation lease owns the last reference after close. */
}

static void *open_raced_owner(void *context)
{
   server_session_capture_t **owner = context;
   *owner = server_session_capture_open("private-child", "raced", "alice", "parent");
   return NULL;
}
static void test_one_live_writer_per_turn(void)
{
   reset();
   pthread_t threads[8];
   server_session_capture_t *opened[8] = {0}, *winner = NULL;
   for (int i = 0; i < 8; i++)
   {
      int spawned = pthread_create(&threads[i], NULL, open_raced_owner, &opened[i]);
      assert(!spawned);
   }
   int accepted = 0;
   for (int i = 0; i < 8; i++)
   {
      int joined = pthread_join(threads[i], NULL);
      assert(!joined);
      if (opened[i])
      {
         winner = opened[i];
         accepted++;
      }
   }
   assert(accepted == 1 && winner);
   server_session_capture_close(winner);
   winner = server_session_capture_open("private-child", "raced", "alice", "parent");
   assert(winner); /* Final lease release removes the live directory entry. */
   server_session_capture_close(winner);
}

int main(void)
{
   server_session_capture_t *absent = NULL;
   int captured = server_session_capture_retain_current(NULL, NULL, &absent);
   assert(!captured && !absent);
   test_concurrent_owner();
   test_host_turn_generation_and_source_claims();
   test_one_live_writer_per_turn();
   test_close_before_callback();
   test_failure_refuses_success();
   test_running_history_source();
   cJSON_Delete(rows);
   puts("retained capture: copied identity, concurrent counters, exact IR, sealed turns and sticky "
        "failure passed");
   return 0;
}
