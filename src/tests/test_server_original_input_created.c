/* Synthetic canonical DB fixture shared with the session logger tests. This
 * test does not install a production human resolver or private signer. */
#include "db1_client/primary_sessions.h"
#define main retained_fixture_main
#include "test_server_session_log.c"
#undef main
#include "../server/host_input_attachment.h"
#include "../server/original_input_created.h"
#include "aimee_sha256.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
const unsigned aimee_original_input_attachment_contract_v1 = 1;
static int executions, admissions, batch_calls, db_mode, installed_rows;
static int post_mode;
static cJSON *minted;
static char *stored_wire;
static char stored_turn[128];
static int random_calls;
/* The linker, rather than a C call site, names this fixture. Keep its exact
 * symbol through LTO in both the ordinary and PostgreSQL link variants. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((externally_visible))
#endif
__attribute__((used)) int
__wrap_RAND_bytes(unsigned char *p, int n)
{
   memset(p, ++random_calls, (size_t)n);
   return 1;
}
void db1_agent_job_set_agent(int n, const char *s)
{
   (void)n;
   (void)s;
}
static int admit(const char *session, const char *turn, const char *principal,
                 const aimee_message_t *m, void *ctx)
{
   (void)turn;
   (void)ctx;
   assert(!strcmp(session, "prototype") && !strcmp(principal, "alice"));
   admissions++;
   return m->blocks[0].input_attachment == minted ? 0 : -1;
}
int db1_session_original_input_begin(const char *session, const char *operation, const char *turn,
                                     const char *actor, const char *wire,
                                     db1_original_input_receipt_t *receipt)
{
   char sequence[32] = {0};
   memset(receipt, 0, sizeof *receipt);
   batch_calls++;
   assert(!strcmp(operation, "12345678-1234-4234-8234-123456789abc"));
   if (stored_wire)
   {
      assert(strcmp(stored_turn, turn));
      return strcmp(stored_wire, wire) ? -2 : 1;
   }
   if (db_mode == 2)
      return -1; /* Unknown without an acknowledged commit. */
   cJSON *batch = cJSON_Parse(wire);
   assert(batch && cJSON_GetArraySize(batch) == 3);
   const cJSON *begin = cJSON_GetObjectItemCaseSensitive(batch, "begin"),
               *input = cJSON_GetObjectItemCaseSensitive(batch, "input");
   char *a = cJSON_PrintUnformatted(begin), *b = cJSON_PrintUnformatted(input);
   assert(a && b);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(begin, "kind")), "execution"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                      cJSON_GetObjectItemCaseSensitive(begin, "payload"), "status")),
                  "running"));
   /* Explicit DB transaction fixture: validate both, then commit their logical
    * original event order together. No PostgreSQL/rollback certification. */
   assert(!db1_session_ir_append(session, "original:begin", turn, actor, a, sequence));
   assert(!db1_session_ir_append(session, "original:input", turn, actor, b, sequence));
   installed_rows += 2;
   receipt->begin_sequence = 1;
   receipt->input_sequence = 2;
   if (post_mode == 7)
      receipt->input_sequence = 4;
   if (post_mode == 8)
   {
      receipt->begin_sequence = UINT64_MAX;
      receipt->input_sequence = 0;
   }
   receipt->contract_version = 1;
   receipt->outcome = DB1_ORIGINAL_INPUT_CREATED;
   snprintf(receipt->original_turn, sizeof receipt->original_turn, "%s", turn);
   stored_wire = strdup(wire);
   snprintf(stored_turn, sizeof stored_turn, "%s", turn);
   free(a);
   free(b);
   cJSON_Delete(batch);
   return db_mode == 1 ? -1 : 0; /* Lost ACK after actual fixture commit. */
}
/* The executor boundary is deliberately represented by this counter. Runtime
 * capture/logger implementations are linked normally; no dispatch copy is used.
 */
static int run(const cJSON *attachment)
{
   server_session_log_t *scope = NULL;
   int rc =
       server_session_log_begin_original("safe original", attachment, "fixture", "", "", &scope);
   if (rc)
   {
      assert(!scope);
      return rc;
   }
   executions++;
   model_reply();
   return server_session_log_finish(scope, 1, NULL);
}
static int owner_cookie, created_calls;
static server_session_capture_t *cancel_owner;
static char cancel_turn[128];
static int cancel_observed;
static const server_input_created_cap_t *saved_cap;
static void cancelled_scope_closed(int success, void *resource)
{
   (void)resource;
   assert(!success);
   server_input_created_view_t v = {0};
   assert(server_input_created_view(saved_cap, &v) == -1);
   cancel_observed++;
}
static int owner_admit(const char *session, const char *turn, const char *principal,
                       const aimee_message_t *m, const void **cookie, void *state)
{
   (void)state;
   assert(session && turn && principal);
   if (m->blocks[0].input_attachment != minted)
      return -1;
   *cookie = post_mode == 4 ? NULL : &owner_cookie;
   return 0;
}
static int created(const server_input_created_cap_t *cap, void *state)
{
   (void)state;
   created_calls++;
   saved_cap = cap;
   server_input_created_view_t v = {0};
   assert(!server_input_created_view(cap, &v));
   assert(v.schema_version == 1 && v.owner_cookie == &owner_cookie &&
          !strcmp(v.session, "prototype") && !strcmp(v.principal, "alice") &&
          !strcmp(v.text, "safe original") && v.text_len == 13 && v.receipt.begin_sequence == 1 &&
          v.receipt.input_sequence == 2 && v.receipt.contract_version == 1 &&
          v.receipt.outcome == DB1_ORIGINAL_INPUT_CREATED);
   // This accessor takes the writer mutex: callback would deadlock under it.
   server_session_capture_counts_t counts = {0};
   assert(!server_session_capture_counts(server_session_capture_current(), &counts) &&
          counts.counter == 2);
   assert(server_input_created_view((const server_input_created_cap_t *)1, &v) == -1);
   if (post_mode == 10)
   {
      assert(!server_session_capture_retain_current("prototype", "alice", &cancel_owner) &&
             cancel_owner);
      snprintf(cancel_turn, sizeof cancel_turn, "%s", server_session_capture_turn(cancel_owner));
      assert(!server_session_capture_after_terminal(cancel_owner, cancelled_scope_closed, NULL));
      assert(!pthread_cancel(pthread_self()));
      pthread_testcancel(); /* Disabled by the handoff: callback returns normally.
                             */
      assert(!server_input_created_view(cap, &v));
   }
   if (post_mode == 5)
   {
      aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "failed"};
      assert(server_session_capture_append(server_session_capture_current(), &event, NULL, NULL) ==
             -1);
   }
   return post_mode == 1 ? 1 : post_mode == 2 ? 2 : 0;
}
static void minted_setup(void)
{
   minted = cJSON_CreateObject();
   char sha[65];
   assert(!aimee_sha256_hex("safe original", 13, sha));
   cJSON_AddNumberToObject(minted, "schema_version", 1);
   cJSON_AddStringToObject(minted, "kind", "host.original-input.v1");
   cJSON_AddStringToObject(minted, "operation_uuid", "12345678-1234-4234-8234-123456789abc");
   cJSON_AddStringToObject(minted, "text_sha256", sha);
   cJSON_AddStringToObject(minted, "opaque", "opaque-source-fixture");
}
static void *cancel_worker(void *unused)
{
   (void)unused;
   attest("prototype", "alice");
   server_session_log_t *scope = NULL;
   (void)server_session_log_begin_original("safe original", minted, "fixture", "", "", &scope);
   assert(!"pending cancellation returned to executor");
   return NULL;
}
static void run_case(int mode)
{
   post_mode = mode;
   attest("prototype", "alice");
   minted_setup();
   assert(!server_input_attachment_install_owner(owner_admit, created, NULL));
   assert(server_input_created_owner_configured());
   assert(server_input_attachment_install(admit, NULL) == -1);
   server_input_created_view_t view = {0};
   assert(server_input_created_view((const server_input_created_cap_t *)1, &view) == -1);
   if (post_mode == 3)
      db_mode = 1;
   if (post_mode == 6)
      db_mode = 2;
   cJSON *supplied = post_mode == 9 ? cJSON_Duplicate(minted, 1) : minted;
   if (post_mode == 10)
   {
      pthread_t thread;
      assert(!pthread_create(&thread, NULL, cancel_worker, NULL));
      void *outcome = NULL;
      assert(!pthread_join(thread, &outcome) && outcome == PTHREAD_CANCELED);
      struct row *r = find("prototype");
      assert(r && cJSON_GetArraySize(r->events) == 2);
      assert(executions == 0 && created_calls == 1 && cancel_observed == 1 && cancel_owner);
      server_session_capture_counts_t counts = {0};
      assert(!server_session_capture_counts(cancel_owner, &counts) && counts.counter == 2);
      assert(!server_session_capture_accepting(cancel_owner));
      assert(server_session_capture_has_owner("prototype", "alice", cancel_turn));
      server_session_capture_release(cancel_owner);
      cancel_owner = NULL;
      assert(!server_session_capture_has_owner("prototype", "alice", cancel_turn));
      assert(!server_session_capture_current() && !server_session_log_active());
      puts("PASS real pthread_cancel: cap cleared, RAM closed, owner released, "
           "events=2, execution=0");
      return;
   }
   int rc = run(supplied);
   struct row *r = find("prototype");
   assert(r);
   if (post_mode == 0)
   {
      assert(!rc && executions == 1 && created_calls == 1 && cJSON_GetArraySize(r->events) == 4);
      assert(server_input_created_view(saved_cap, &view) == -1);
      rc = run(minted);
      assert(rc < 0 && executions == 1 && created_calls == 1 && cJSON_GetArraySize(r->events) == 4);
   }
   else
   {
      assert(rc < 0 && executions == 0);
      int rows = post_mode == 4 || post_mode == 6 || post_mode == 9 ? 0 : 2;
      assert(cJSON_GetArraySize(r->events) == rows);
      assert(created_calls == (post_mode == 1 || post_mode == 2 || post_mode == 5 ? 1 : 0));
      assert(server_input_created_view(saved_cap, &view) == -1);
      assert(!server_session_capture_current() && !server_session_log_active());
   }
   printf("PASS post56 case%d callback=%d execution=%d events=%d\n", post_mode, created_calls,
          executions, cJSON_GetArraySize(r->events));
}

/* Startup installation is process-wide and immutable. Isolate each case using
 * fork, retaining ordinary zero-argument unit-test runner semantics. */
int main(void)
{
   for (int mode = 0; mode < 11; mode++)
   {
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         alarm(5);
         run_case(mode);
         fflush(stdout);
         _exit(0);
      }
      int status = 0;
      assert(waitpid(child, &status, 0) == child);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   puts("PASS original-input post-created protocol and cancellation (11 cases)");
   return 0;
}
