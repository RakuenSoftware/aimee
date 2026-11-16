/* Generic capabilities with synthetic DB and target callbacks, not Native custody. */
#define SERVER_INPUT_DELIVERY_TEST_ENTRY delivery_legacy_entry
#include "test_server_original_input_delivery.c"
#include "../server/original_input_plan.h"
const unsigned aimee_original_input_safe_plan_client_contract_v1 = 1;
static db1_original_input_plan_receipt_t plan_receipt;
static int plan_case, plan_probe_calls, plan_reserve_calls, planned_calls, terminal_calls,
    terminal_acks;
static const server_input_pending_plan_cap_t *saved_pending;
static db1_original_input_probe_t cancel_probe;
void db1_original_input_probe_destroy(db1_original_input_probe_t *p)
{
   free(p->opaque_plan);
   free(p->terminal_result);
   memset(p, 0, sizeof *p);
}
int db1_session_original_input_probe(const char *s, const char *op, const char *actor,
                                     db1_original_input_probe_t *out)
{
   assert(!strcmp(s, "prototype") && !strcmp(op, "12345678-1234-4234-8234-123456789abc") &&
          !strcmp(actor, "alice"));
   plan_probe_calls++;
   memset(out, 0, sizeof *out);
   out->contract_version = 1;
   out->presence = plan_case == 5   ? DB1_INPUT_PROBE_TERMINAL
                   : plan_case == 6 ? DB1_INPUT_PROBE_COMMITTED
                                    : DB1_INPUT_PROBE_RESERVED;
   out->plan = plan_receipt;
   out->opaque_plan = strdup("{}");
   out->opaque_plan_len = 2;
   out->terminal_result = strdup("");
   if (plan_case == 8)
      assert(!pthread_cancel(pthread_self()));
   return 0;
}
int db1_session_original_input_plan_reserve(const char *s, const char *op, const char *a,
                                            const char *receiver, const char *text,
                                            const char *hash, const char *opaque,
                                            db1_original_input_plan_receipt_t *out)
{
   assert(!strcmp(s, "prototype") && !strcmp(a, "alice") && !strcmp(receiver, "source-owner") &&
          !strcmp(opaque, "{}"));
   plan_reserve_calls++;
   memset(&plan_receipt, 0, sizeof plan_receipt);
   plan_receipt.contract_version = 1;
   plan_receipt.plan_id = 9007199254740993ULL;
   plan_receipt.state = DB1_INPUT_PLAN_RESERVED;
   strcpy(plan_receipt.session, s);
   strcpy(plan_receipt.operation_uuid, op);
   strcpy(plan_receipt.actor, a);
   strcpy(plan_receipt.receiver_tag, receiver);
   strcpy(plan_receipt.safe_text_sha256, text);
   strcpy(plan_receipt.plan_sha256, hash);
   *out = plan_receipt;
   return 0;
}
int db1_session_original_input_begin_planned_delivery(const char *s, const char *op,
                                                      const char *turn, const char *a,
                                                      const char *wire, const char *intent,
                                                      const char *id, const char *hash,
                                                      db1_original_input_receipt_t *out,
                                                      uint64_t *did)
{
   planned_calls++;
   assert(!strcmp(id, "9007199254740993") && !strcmp(hash, plan_receipt.plan_sha256));
   return db1_session_original_input_begin_delivery(s, op, turn, a, wire, intent, out, did);
}
int db1_session_original_input_plan_terminal(const char *id, const char *s, const char *op,
                                             const char *a, const char *r, const char *hash,
                                             const char *result)
{
   assert(!strcmp(id, "9007199254740993") && !strcmp(s, "prototype") && strlen(op) == 36 &&
          !strcmp(a, "alice") && !strcmp(r, "source-owner") &&
          !strcmp(hash, plan_receipt.plan_sha256) &&
          !strcmp(result, "{\"fixture_verified_closed\":true}"));
   terminal_acks++;
   return 0;
}
static int check_owner(const void *cookie, server_input_plan_owner_view_t *v, void *state)
{
   (void)state;
   if (cookie != &owner_cookie || plan_case == 1)
      return -1;
   *v = (server_input_plan_owner_view_t){1, "prototype", "12345678-1234-4234-8234-123456789abc",
                                         "alice"};
   return 0;
}
static int plan_prepare(const server_input_prepared_cap_t *cap, char **wire, void *state)
{
   assert(!prepare_delivery(cap, wire, state));
   if (plan_case == 2)
      return 0;
   db1_original_input_plan_receipt_t wrong = plan_receipt;
   strcpy(wrong.actor, "bob");
   assert(server_input_prepared_bind_plan_v1(cap, &wrong) == -1);
   assert(!server_input_prepared_bind_plan_v1(cap, &plan_receipt));
   assert(server_input_prepared_bind_plan_v1(cap, &plan_receipt) == -1);
   return 0;
}
static int terminal(const server_input_pending_plan_cap_t *cap, char **result, void *state)
{
   (void)state;
   server_input_pending_plan_view_t v = {0};
   assert(!server_input_pending_plan_view(cap, &v) && v.receipt.plan_id == 9007199254740993ULL &&
          v.opaque_plan_len == 2 && !strcmp(v.opaque_plan, "{}"));
   server_input_created_view_t c = {0};
   assert(server_input_created_view((void *)cap, &c) == -1);
   saved_pending = cap;
   terminal_calls++;
   if (plan_case == 7)
      return -1;
   *result = strdup("{\"fixture_verified_closed\":true}");
   return 0;
}
static void *cancel_probe_worker(void *unused)
{
   (void)unused;
   (void)server_input_plan_probe_v1(&owner_cookie, &cancel_probe);
   assert(!"probe cancellation returned");
   return NULL;
}
static void plan_run(int mode)
{
   plan_case = mode;
   attest("prototype", "alice");
   minted_setup();
   assert(!server_input_attachment_install_owner(owner_admit, delivery_created, NULL));
   assert(!server_input_delivery_install_owner_v2("source-owner", "alice", plan_prepare,
                                                  recover_delivery, check_owner, terminal, NULL));
   char text[65], hash[65];
   assert(!aimee_sha256_hex("safe original", 13, text) && !aimee_sha256_hex("{}", 2, hash));
   db1_original_input_probe_t probe = {0};
   assert(server_input_plan_probe_v1((void *)1, &probe) == -1 && plan_probe_calls == 0);
   db1_original_input_plan_receipt_t receipt = {0};
   int rc = server_input_plan_reserve_v1(&owner_cookie, text, hash, "{}", &receipt);
   if (mode == 1)
   {
      assert(rc == -1 && !plan_reserve_calls);
      return;
   }
   assert(!rc && receipt.plan_id == 9007199254740993ULL);
   if (mode == 8)
   {
      pthread_t thread;
      assert(!pthread_create(&thread, NULL, cancel_probe_worker, NULL));
      void *status = NULL;
      assert(!pthread_join(thread, &status) && status == PTHREAD_CANCELED);
      assert(!cancel_probe.opaque_plan && !cancel_probe.terminal_result &&
             !cancel_probe.contract_version && !terminal_calls && !terminal_acks);
      return;
   }
   assert(!server_input_plan_probe_v1(&owner_cookie, &probe) && probe.opaque_plan);
   db1_original_input_probe_destroy(&probe);
   if (mode <= 2)
   {
      rc = run(minted);
      if (mode == 2)
         assert(rc < 0 && planned_calls == 0 && created_calls == 0 && executions == 0);
      else
         assert(!rc && planned_calls == 1 && created_calls == 1 && executions == 1);
      assert(server_input_prepared_bind_plan_v1(saved_prepared, &receipt) == -1);
      return;
   }
   if (mode == 3)
   {
      rc = server_input_plan_conclude_v1(
          "prototype", receipt.operation_uuid,
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
      assert(rc == DB1_INPUT_PLAN_CONFLICT && terminal_calls == 0 && terminal_acks == 0);
      return;
   }
   rc = server_input_plan_conclude_v1("prototype", receipt.operation_uuid, hash);
   if (mode == 5)
      assert(rc == 1 && terminal_calls == 0 && terminal_acks == 0);
   else if (mode == 6)
      assert(rc == DB1_INPUT_PLAN_CONFLICT && terminal_calls == 0 && terminal_acks == 0);
   else if (mode == 7)
      assert(rc == -1 && terminal_calls == 1 && terminal_acks == 0);
   else
      assert(rc == 1 && terminal_calls == 1 && terminal_acks == 1);
   server_input_pending_plan_view_t v = {0};
   assert(server_input_pending_plan_view(saved_pending, &v) == -1);
   assert(executions == 0 && created_calls == 0 && planned_calls == 0);
}
int main(void)
{
   for (int mode = 0; mode < 9; mode++)
   {
      pid_t p = fork();
      assert(p >= 0);
      if (!p)
      {
         plan_run(mode);
         printf("PASS generic plan capability case%d\n", mode);
         fflush(stdout);
         _exit(0);
      }
      int status;
      assert(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   puts("PASS 9 capability cases; synthetic target proof only, no Raw2/Native effects");
   return 0;
}
