#define main retained_chat_fixture_main
#include "test_server_chat_ir.c"
#undef main
#include "server/host_input_attachment.h"
#include "server/original_input_created.h"
#include "aimee_sha256.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
const unsigned aimee_original_input_attachment_contract_v1 = 1;
static int mode, batches, callbacks, model_runs;
static cJSON *minted;
static int cookie;
static const char *expected_operation = "12345678-1234-4234-8234-123456789abc";
static int admit(const char *session, const char *turn, const char *principal,
                 const aimee_message_t *m, const void **out, void *state)
{
   (void)turn;
   (void)state;
   assert(!strcmp(session, "session-one") && !strcmp(principal, "alice"));
   if (mode == 2 || m->blocks[0].input_attachment != minted)
      return -1;
   *out = mode == 11 ? NULL : &cookie;
   return 0;
}
static int created(const server_input_created_cap_t *cap, void *state)
{
   (void)state;
   server_input_created_view_t v = {0};
   callbacks++;
   assert(!server_input_created_view(cap, &v) && v.owner_cookie == &cookie);
   server_session_capture_counts_t counts;
   assert(!server_session_capture_counts(server_session_capture_current(), &counts) &&
          counts.counter == 2);
   if (mode == 7)
      assert(!pthread_cancel(pthread_self()));
   if (mode == 12)
   {
      aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = "completed"};
      assert(server_session_capture_append(server_session_capture_current(), &event, NULL, NULL) ==
             -1);
   }
   if (mode == 15)
      fail_store = 1; /* BEGIN/User committed; terminal storage becomes unavailable. */
   return mode == 6 || mode == 15 ? -1 : 0;
}
int db1_session_original_input_begin(const char *session, const char *operation, const char *turn,
                                     const char *actor, const char *wire,
                                     db1_original_input_receipt_t *receipt)
{
   batches++;
   assert(!strcmp(operation, expected_operation));
   if (mode == 3)
      return 1;
   if (mode == 5)
      return -1;
   cJSON *batch = cJSON_Parse(wire);
   assert(batch);
   char sequence[32];
   const char *keys[] = {"begin", "input"};
   for (int i = 0; i < 2; i++)
   {
      char *event = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(batch, keys[i]));
      assert(event && !db1_session_ir_append(session, keys[i], turn, actor, event, sequence));
      free(event);
   }
   cJSON_Delete(batch);
   *receipt = (db1_original_input_receipt_t){
       .contract_version = 1,
       .outcome = DB1_ORIGINAL_INPUT_CREATED,
       .begin_sequence = (uint64_t)cJSON_GetArraySize(log_entries) - 1,
       .input_sequence = (uint64_t)cJSON_GetArraySize(log_entries)};
   snprintf(receipt->original_turn, sizeof receipt->original_turn, "%s", turn);
   if (mode == 8)
      receipt->input_sequence = 4;
   if (mode == 9)
   {
      receipt->begin_sequence = UINT64_MAX;
      receipt->input_sequence = 0;
   }
   return mode == 4 ? -1 : 0;
}
static cJSON *attachment(void)
{
   cJSON *a = cJSON_CreateObject();
   char digest[65];
   assert(a && !aimee_sha256_hex("safe input", 10, digest));
   cJSON_AddNumberToObject(a, "schema_version", 1);
   cJSON_AddStringToObject(a, "kind", "host.original-input.v1");
   cJSON_AddStringToObject(a, "operation_uuid", expected_operation);
   cJSON_AddStringToObject(
       a, "text_sha256",
       mode == 10 ? "0000000000000000000000000000000000000000000000000000000000000000" : digest);
   cJSON_AddStringToObject(a, "opaque", "opaque safe metadata");
   return a;
}
static void *worker(void *pointer)
{
   compute_ctx_t *c = pointer;
   int result = server_chat_ir_begin_original(c, "session-one", "safe input", minted);
   if (result == 0)
      model_runs++;
   return (void *)(long)result;
}
static void run(int selected)
{
   mode = selected;
   log_entries = cJSON_CreateArray();
   assert(log_entries);
   minted = attachment();
   compute_ctx_t c = context();
   if (mode != 0)
      assert(!server_input_attachment_install_owner(admit, created, NULL));
   if (mode == 3)
   {
      cJSON_AddItemToArray(log_entries, cJSON_CreateObject());
      cJSON_AddItemToArray(log_entries, cJSON_CreateObject());
   }
   if (mode == 13)
   {
      for (int i = 0; i < 200; i++)
         cJSON_AddItemToArray(log_entries, cJSON_CreateObject());
      delivered_sequence = 200; /* Previous turns already rendered. */
   }
   fail_page = mode == 14;
   if (mode == 7)
   {
      pthread_t t;
      void *result;
      assert(!pthread_create(&t, NULL, worker, &c) && !pthread_join(t, &result) &&
             result == PTHREAD_CANCELED);
      assert(!c.ir_enabled && !c.ir_capture && !c.ir_controls && callbacks == 1 && !model_runs &&
             cJSON_GetArraySize(log_entries) == 3);
      const cJSON *terminal = cJSON_GetArrayItem(log_entries, 2);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(terminal, "kind")),
                     "execution"));
      const cJSON *payload = cJSON_GetObjectItemCaseSensitive(terminal, "payload");
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "status")),
                     "cancelled"));
   }
   else
   {
      long result = (long)worker(&c);
      if (mode == 1 || mode == 13)
      {
         int prior = mode == 13 ? 200 : 0;
         assert(!result && callbacks == 1 && model_runs == 1 &&
                cJSON_GetArraySize(log_entries) == prior + 2);
         assert(writes == 2 && delivered_sequence == prior + 2);
         char before[32], expected_before[32];
         snprintf(expected_before, sizeof expected_before, "%d", prior);
         assert(!server_session_capture_original_input_before(c.ir_capture, before) &&
                !strcmp(before, expected_before));
         aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "later callback"};
         aimee_message_t message = {.role = "assistant", .blocks = &text, .n_blocks = 1};
         aimee_session_event_t later = {
             .kind = AIMEE_SESSION_MESSAGE, .message_id = "later", .message = &message};
         assert(!server_session_capture_sink(&later, c.ir_capture));
         assert(!server_session_capture_original_input_before(c.ir_capture, before) &&
                !strcmp(before, expected_before));
         assert(!server_chat_ir_progress(&c) && writes == 3 && delivered_sequence == prior + 3);
         server_chat_ir_abort(&c);
         assert(cJSON_GetArraySize(log_entries) == prior + 3);
      }
      else
      {
         assert(result != 0 && !model_runs && !c.ir_enabled && !c.ir_capture && !c.ir_controls);
         int expected = (mode == 3 || mode == 4 || mode == 6 || mode == 8 || mode == 9 ||
                         mode == 12 || mode == 14 || mode == 15)
                            ? 2
                            : 0;
         if (mode == 6 || mode == 12 || mode == 14)
            expected = 3;
         assert(cJSON_GetArraySize(log_entries) == expected);
         if (expected == 3)
         {
            const cJSON *terminal = cJSON_GetArrayItem(log_entries, 2);
            const cJSON *payload = cJSON_GetObjectItemCaseSensitive(terminal, "payload");
            assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(terminal, "kind")),
                           "execution"));
            assert(
                !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "status")),
                        "failed"));
         }
         assert(callbacks == ((mode == 6 || mode == 12 || mode == 14 || mode == 15) ? 1 : 0));
      }
   }
   assert(!server_session_capture_current());
   cJSON_Delete(minted);
   cJSON_Delete(log_entries);
   printf("PASS atomic original chat case %d\n", mode);
}
/* Original operation replay compares immutable batch bytes. Distinct turns keep
 * their canonical attachment identity while the legacy outer ID stays stable;
 * consumers must scope that legacy Original ID to its envelope turn. */
static void original_input_multiturn_wire_identity(void)
{
   mode = 1;
   log_entries = cJSON_CreateArray();
   assert(log_entries && !server_input_attachment_install_owner(admit, created, NULL));
   char turns[2][192] = {{0}};
   for (int i = 0; i < 2; ++i)
   {
      expected_operation =
          i ? "22345678-1234-4234-8234-123456789abc" : "12345678-1234-4234-8234-123456789abc";
      minted = attachment();
      compute_ctx_t c = context();
      assert(!(long)worker(&c));
      assert(cJSON_GetArraySize(log_entries) == (i + 1) * 2);
      const cJSON *event = cJSON_GetArrayItem(log_entries, i * 2 + 1);
      const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "message_id"));
      assert(id && !strcmp(id, "user"));
      snprintf(turns[i], sizeof turns[i], "%s", c.ir_turn);
      aimee_message_t decoded = {0};
      assert(!aimee_ir_message_parse(cJSON_GetObjectItemCaseSensitive(event, "payload"), &decoded));
      assert(decoded.role && !strcmp(decoded.role, "user") && decoded.n_blocks == 1 &&
             !strcmp(decoded.blocks[0].text, "safe input") &&
             cJSON_Compare(decoded.blocks[0].input_attachment, minted, 1));
      aimee_message_free(&decoded);
      server_chat_ir_abort(&c);
      assert(!server_session_capture_current());
      cJSON_Delete(minted);
      minted = NULL;
   }
   assert(strcmp(turns[0], turns[1]) && callbacks == 2 && model_runs == 2 && batches == 2);
   assert(writes == 4 && delivered_sequence == 4);
   cJSON_Delete(log_entries);
   puts("PASS atomic Original multi-turn immutable wire identities and canonical payloads");
}

#ifndef AIMEE_CHAT_ORIGINAL_TEST_MAIN
#define AIMEE_CHAT_ORIGINAL_TEST_MAIN main
#endif
int AIMEE_CHAT_ORIGINAL_TEST_MAIN(void)
{
   for (int i = 0; i < 16; i++)
   {
      pid_t p = fork();
      assert(p >= 0);
      if (!p)
      {
         alarm(5);
         run(i);
         exit(0);
      }
      int status;
      assert(waitpid(p, &status, 0) == p && WIFEXITED(status) && !WEXITSTATUS(status));
   }
   {
      pid_t p = fork();
      assert(p >= 0);
      if (!p)
      {
         alarm(5);
         original_input_multiturn_wire_identity();
         exit(0);
      }
      int status;
      assert(waitpid(p, &status, 0) == p && WIFEXITED(status) && !WEXITSTATUS(status));
   }
   pid_t p = fork();
   assert(p >= 0);
   if (!p)
   {
      alarm(15);
      exit(retained_chat_fixture_main());
   }
   int status;
   assert(waitpid(p, &status, 0) == p && WIFEXITED(status) && !WEXITSTATUS(status));
   puts("PASS atomic chat original input (16 cases) and unchanged legacy fixture");
   return 0;
}
