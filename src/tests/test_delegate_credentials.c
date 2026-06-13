/* test_delegate_credentials.c: per-provider credential lease pool. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "delegate_credentials.h"

static agent_credential_t make_cred(const char *name, const char *env_var)
{
   agent_credential_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.name, sizeof(c.name), "%s", name);
   snprintf(c.api_key_env, sizeof(c.api_key_env), "%s", env_var);
   return c;
}

static void test_acquire_round_robin(void)
{
   delegate_credentials_reset_for_test();

   agent_credential_t creds[2];
   creds[0] = make_cred("main", "OPENROUTER_API_KEY");
   creds[1] = make_cred("backup", "OPENROUTER_API_KEY_2");

   char a_name[32] = {0}, a_env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, a_name, sizeof(a_name), a_env,
                                       sizeof(a_env)) == 0);
   assert(strcmp(a_name, "main") == 0);

   /* Second call leases the next credential, not the same one. */
   char b_name[32] = {0}, b_env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, b_name, sizeof(b_name), b_env,
                                       sizeof(b_env)) == 0);
   assert(strcmp(b_name, "backup") == 0);

   /* Third call: pool exhausted. */
   char c_name[32] = {0}, c_env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, c_name, sizeof(c_name), c_env,
                                       sizeof(c_env)) == -1);

   printf("  PASS: test_acquire_round_robin\n");
}

static void test_release_makes_credential_available(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[2];
   creds[0] = make_cred("main", "OPENROUTER_API_KEY");
   creds[1] = make_cred("backup", "OPENROUTER_API_KEY_2");

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == -1);

   delegate_credentials_release("", "openrouter", "main");

   /* Now there is one available again. */
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(strcmp(name, "main") == 0);
   printf("  PASS: test_release_makes_credential_available\n");
}

static void test_cooldown_skips_credential(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[2];
   creds[0] = make_cred("main", "OPENROUTER_API_KEY");
   creds[1] = make_cred("backup", "OPENROUTER_API_KEY_2");

   /* Cool main; even before any acquire happens, main is unavailable. */
   delegate_credentials_cooldown("", "openrouter", "main", 60);

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(strcmp(name, "backup") == 0);

   /* No more credentials available — main is still cooling. */
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == -1);
   printf("  PASS: test_cooldown_skips_credential\n");
}

static void test_capture_headers_records_quota_windows(void)
{
   delegate_credentials_reset_for_test();
   const char *headers = "HTTP/1.1 200 OK\r\n"
                         "x-ratelimit-limit-requests: 100\r\n"
                         "x-ratelimit-remaining-requests: 7\r\n"
                         "x-ratelimit-reset-requests: 30\r\n"
                         "x-ratelimit-limit-tokens: 10000\r\n"
                         "x-ratelimit-remaining-tokens: 2500\r\n"
                         "x-ratelimit-reset-tokens: 45\r\n";
   assert(delegate_credentials_capture_headers("", "openrouter", "main", headers, 1000) == 0);

   delegate_credential_snapshot_t rows[4];
   int n = delegate_credentials_snapshot("openrouter", rows, 4);
   assert(n == 1);
   assert(strcmp(rows[0].cred_name, "main") == 0);
   assert(rows[0].rl.req_limit == 100);
   assert(rows[0].rl.req_remaining == 7);
   assert(rows[0].rl.req_reset == 1030);
   assert(rows[0].rl.tok_limit == 10000);
   assert(rows[0].rl.tok_remaining == 2500);
   assert(rows[0].rl.tok_reset == 1045);
   printf("  PASS: test_capture_headers_records_quota_windows\n");
}

static void test_acquire_prefers_freshest_quota(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[2];
   creds[0] = make_cred("low", "LOW_KEY");
   creds[1] = make_cred("fresh", "FRESH_KEY");

   assert(delegate_credentials_capture_headers(
              "", "openrouter", "low",
              "x-ratelimit-remaining-requests: 2\nx-ratelimit-remaining-tokens: 500\n", 1000) == 0);
   assert(delegate_credentials_capture_headers(
              "", "openrouter", "fresh",
              "x-ratelimit-remaining-requests: 50\nx-ratelimit-remaining-tokens: 8000\n",
              1000) == 0);

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(strcmp(name, "fresh") == 0);
   assert(strcmp(env, "FRESH_KEY") == 0);
   printf("  PASS: test_acquire_prefers_freshest_quota\n");
}

static void test_report_failure_marks_and_rotates(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[2];
   creds[0] = make_cred("main", "MAIN_KEY");
   creds[1] = make_cred("backup", "BACKUP_KEY");

   assert(delegate_credentials_capture_headers(
              "", "openrouter", "main",
              "x-ratelimit-reset-requests: 120\nx-ratelimit-remaining-requests: 0\n", 1000) == 0);
   assert(delegate_credentials_report_failure("", "openrouter", "main", FAILOVER_RATE_LIMIT,
                                              "HTTP 429", 1000) == 1);

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(strcmp(name, "backup") == 0);

   delegate_credential_snapshot_t rows[4];
   int n = delegate_credentials_snapshot("openrouter", rows, 4);
   assert(n == 2);
   int found = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].cred_name, "main") == 0)
      {
         found = 1;
         assert(rows[i].status == DELEGATE_CRED_STATUS_RATE_LIMITED);
         assert(rows[i].cooldown_until == 1120);
      }
   assert(found);
   printf("  PASS: test_report_failure_marks_and_rotates\n");
}

static void test_rotate_after_failure_releases_and_leases_next(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[2];
   creds[0] = make_cred("main", "MAIN_KEY");
   creds[1] = make_cred("backup", "BACKUP_KEY");

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(strcmp(name, "main") == 0);
   assert(delegate_credentials_capture_headers(
              "", "openrouter", "main",
              "x-ratelimit-reset-requests: 120\nx-ratelimit-remaining-requests: 0\n", 1000) == 0);

   assert(delegate_credentials_rotate_after_failure("", "openrouter", creds, 2, "openrouter", name,
                                                    sizeof(name), env, sizeof(env), "HTTP 429",
                                                    1000) == 1);
   assert(strcmp(name, "backup") == 0);
   assert(strcmp(env, "BACKUP_KEY") == 0);

   delegate_credential_snapshot_t rows[4];
   int n = delegate_credentials_snapshot("openrouter", rows, 4);
   assert(n == 2);
   int saw_main = 0, saw_backup = 0;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(rows[i].cred_name, "main") == 0)
      {
         saw_main = 1;
         assert(rows[i].status == DELEGATE_CRED_STATUS_RATE_LIMITED);
         assert(rows[i].leased == 0);
      }
      if (strcmp(rows[i].cred_name, "backup") == 0)
      {
         saw_backup = 1;
         assert(rows[i].leased == 1);
      }
   }
   assert(saw_main);
   assert(saw_backup);
   printf("  PASS: test_rotate_after_failure_releases_and_leases_next\n");
}

static void test_rotate_after_non_pool_failure_keeps_lease(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[2];
   creds[0] = make_cred("main", "MAIN_KEY");
   creds[1] = make_cred("backup", "BACKUP_KEY");

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(delegate_credentials_rotate_after_failure("", "openrouter", creds, 2, "openrouter", name,
                                                    sizeof(name), env, sizeof(env),
                                                    "connection refused", 1000) == 0);
   assert(strcmp(name, "main") == 0);
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(strcmp(name, "backup") == 0);
   printf("  PASS: test_rotate_after_non_pool_failure_keeps_lease\n");
}

static void test_billing_and_auth_failures_are_not_reacquired(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[2];
   creds[0] = make_cred("billing", "BILLING_KEY");
   creds[1] = make_cred("auth", "AUTH_KEY");

   assert(delegate_credentials_report_failure("", "openrouter", "billing", FAILOVER_BILLING,
                                              "payment required", 1000) == 1);
   assert(delegate_credentials_report_failure("", "openrouter", "auth", FAILOVER_AUTH_PERMANENT,
                                              "forbidden", 1000) == 1);

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 2, name, sizeof(name), env,
                                       sizeof(env)) == -1);
   char quota[512];
   assert(delegate_credentials_format_quota("openrouter", quota, sizeof(quota)) == 2);
   assert(strstr(quota, "status=exhausted") != NULL);
   assert(strstr(quota, "status=auth_failed") != NULL);
   printf("  PASS: test_billing_and_auth_failures_are_not_reacquired\n");
}

static void test_health_state_persists_across_reset(void)
{
   delegate_credentials_reset_for_test();
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-test-credential-state-%ld.tsv", (long)getpid());

   agent_credential_t creds[3];
   creds[0] = make_cred("main", "MAIN_KEY");
   creds[1] = make_cred("billing", "BILLING_KEY");
   creds[2] = make_cred("fresh", "FRESH_KEY");

   assert(delegate_credentials_capture_headers(
              "", "openrouter", "main",
              "x-ratelimit-reset-requests: 120\nx-ratelimit-remaining-requests: 0\n", 1000) == 0);
   assert(delegate_credentials_report_failure("", "openrouter", "main", FAILOVER_RATE_LIMIT,
                                              "HTTP 429", 1000) == 1);
   assert(delegate_credentials_report_failure("", "openrouter", "billing", FAILOVER_BILLING,
                                              "payment required", 1000) == 1);
   assert(delegate_credentials_save_file(path) == 0);

   delegate_credentials_reset_for_test();
   assert(delegate_credentials_load_file(path, 1000) == 2);

   char name[32] = {0}, env[64] = {0};
   assert(delegate_credentials_acquire("", "openrouter", creds, 3, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(strcmp(name, "fresh") == 0);

   delegate_credential_snapshot_t rows[4];
   int n = delegate_credentials_snapshot("openrouter", rows, 4);
   assert(n == 3);
   int saw_rate_limited = 0;
   int saw_exhausted = 0;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(rows[i].cred_name, "main") == 0)
      {
         saw_rate_limited = 1;
         assert(rows[i].status == DELEGATE_CRED_STATUS_RATE_LIMITED);
         assert(rows[i].cooldown_until == 1120);
         assert(rows[i].leased == 0);
      }
      if (strcmp(rows[i].cred_name, "billing") == 0)
      {
         saw_exhausted = 1;
         assert(rows[i].status == DELEGATE_CRED_STATUS_EXHAUSTED);
      }
   }
   assert(saw_rate_limited);
   assert(saw_exhausted);
   unlink(path);
   printf("  PASS: test_health_state_persists_across_reset\n");
}

static void test_isolation_between_agents(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[1];
   creds[0] = make_cred("main", "AGENT_KEY");

   char name[32] = {0}, env[64] = {0};
   /* agent A leases main. agent B's "main" is a different state row,
    * so it's independent and still available. */
   assert(delegate_credentials_acquire("", "agent-A", creds, 1, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   assert(delegate_credentials_acquire("", "agent-A", creds, 1, name, sizeof(name), env,
                                       sizeof(env)) == -1);
   assert(delegate_credentials_acquire("", "agent-B", creds, 1, name, sizeof(name), env,
                                       sizeof(env)) == 0);
   printf("  PASS: test_isolation_between_agents\n");
}

static void test_lease_state_grows_beyond_initial_capacity(void)
{
   delegate_credentials_reset_for_test();
   agent_credential_t creds[1];
   creds[0] = make_cred("main", "AGENT_KEY");

   for (int i = 0; i < 80; i++)
   {
      char agent[32];
      char name[32] = {0}, env[64] = {0};
      snprintf(agent, sizeof(agent), "agent-%02d", i);
      assert(delegate_credentials_acquire("", agent, creds, 1, name, sizeof(name), env,
                                          sizeof(env)) == 0);
      assert(strcmp(name, "main") == 0);
   }
   printf("  PASS: test_lease_state_grows_beyond_initial_capacity\n");
}

static void test_empty_pool_returns_error(void)
{
   delegate_credentials_reset_for_test();
   char name[32] = {0}, env[64] = {0};
   /* credential_count == 0 means "no pool" — caller falls back to api_key. */
   assert(delegate_credentials_acquire("", "agent-A", NULL, 0, name, sizeof(name), env,
                                       sizeof(env)) == -1);
   printf("  PASS: test_empty_pool_returns_error\n");
}

static void test_is_rate_limit_classifier(void)
{
   /* Triggers cooldown — shape matches what providers return on quota
    * exhaustion or upstream overload. */
   assert(delegate_credentials_is_rate_limit("HTTP 429 too many requests") == 1);
   assert(delegate_credentials_is_rate_limit("rate limit exceeded") == 1);
   assert(delegate_credentials_is_rate_limit("error: rate_limit_error") == 1);
   assert(delegate_credentials_is_rate_limit("RATE_LIMIT_EXCEEDED") == 1);
   assert(delegate_credentials_is_rate_limit("upstream overloaded") == 1);

   /* Does not trigger cooldown — generic / unrelated failure modes
    * should not poison the credential pool. */
   assert(delegate_credentials_is_rate_limit("connection refused") == 0);
   assert(delegate_credentials_is_rate_limit("HTTP 500 internal error") == 0);
   assert(delegate_credentials_is_rate_limit("model not found") == 0);
   assert(delegate_credentials_is_rate_limit("") == 0);
   assert(delegate_credentials_is_rate_limit(NULL) == 0);
   printf("  PASS: test_is_rate_limit_classifier\n");
}

static void test_failure_classifier_maps_pool_reasons(void)
{
   assert(delegate_credentials_classify_failure("openrouter", "HTTP 429 too many requests") ==
          FAILOVER_RATE_LIMIT);
   assert(delegate_credentials_classify_failure("openrouter", "HTTP 402 payment required") ==
          FAILOVER_BILLING);
   assert(delegate_credentials_classify_failure("openrouter", "HTTP 401 unauthorized") ==
          FAILOVER_AUTH);
   assert(delegate_credentials_classify_failure("openrouter", "HTTP 403 forbidden") ==
          FAILOVER_AUTH_PERMANENT);
   assert(delegate_credentials_classify_failure("openrouter", "insufficient quota") ==
          FAILOVER_BILLING);
   assert(delegate_credentials_classify_failure("openrouter", "connection refused") ==
          FAILOVER_NONE);
   assert(delegate_credentials_classify_failure("openrouter", NULL) == FAILOVER_NONE);
   printf("  PASS: test_failure_classifier_maps_pool_reasons\n");
}

/* WP-C.3: a 429 cooling one principal's vaulted credential must not cool the
 * same (agent, cred) for another principal, nor the shared "" env pool. */
static void test_principal_cooldown_isolation(void)
{
   delegate_credentials_reset_for_test();
   delegate_credentials_cooldown("uid:alice", "claude", "api_key", 60);

   assert(delegate_credentials_cooldown_remaining("uid:alice", "claude", "api_key", 0) > 0);
   assert(delegate_credentials_cooldown_remaining("uid:bob", "claude", "api_key", 0) == 0);
   assert(delegate_credentials_cooldown_remaining("", "claude", "api_key", 0) == 0);
   /* NULL principal normalises to the shared "" pool — still independent. */
   assert(delegate_credentials_cooldown_remaining(NULL, "claude", "api_key", 0) == 0);
   printf("  PASS: test_principal_cooldown_isolation\n");
}

/* WP-C.3: cooldown_remaining reports the live (monotonic) seconds left, returns 0
 * for an unknown row without creating one, and 0 once a restored deadline passes. */
static void test_cooldown_remaining_reports_seconds(void)
{
   delegate_credentials_reset_for_test();
   assert(delegate_credentials_report_failure("webuser:carol", "claude", "api_key",
                                              FAILOVER_RATE_LIMIT, "HTTP 429", 0) == 1);
   int rem = delegate_credentials_cooldown_remaining("webuser:carol", "claude", "api_key", 0);
   assert(rem > 0 && rem <= DELEGATE_CRED_COOLDOWN_SECONDS_DEFAULT);
   /* Unknown row -> 0, and no row was created. */
   assert(delegate_credentials_cooldown_remaining("uid:nobody", "claude", "api_key", 0) == 0);

   /* A persisted, per-principal rate-limited row whose deadline has already passed
    * loads as not-cooling (load re-bases the monotonic timer off the epoch). */
   delegate_credentials_reset_for_test();
   char p[256];
   snprintf(p, sizeof(p), "/tmp/aimee-test-cd-%ld.tsv", (long)getpid());
   FILE *fp = fopen(p, "w");
   assert(fp != NULL);
   fputs("aimee-delegate-credential-state-v2\nuid:carol\tclaude\tapi_key\t1\t1500\n", fp);
   fclose(fp);
   assert(delegate_credentials_load_file(p, 5000) == 1); /* now 5000 > deadline 1500 */
   assert(delegate_credentials_cooldown_remaining("uid:carol", "claude", "api_key", 5000) == 0);
   unlink(p);
   printf("  PASS: test_cooldown_remaining_reports_seconds\n");
}

/* WP-C.3: the v2 state file round-trips the principal column, keeping per-principal
 * rows distinct from the shared "" pool; legacy v1 files load with principal "". */
static void test_save_load_v2_roundtrips_principal(void)
{
   delegate_credentials_reset_for_test();
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-test-cred-v2-%ld.tsv", (long)getpid());

   /* A shared-pool row and a per-principal vault row, both rate-limited. */
   assert(delegate_credentials_report_failure("", "openrouter", "main", FAILOVER_RATE_LIMIT,
                                              "HTTP 429", 1000) == 1);
   assert(delegate_credentials_report_failure("uid:alice", "claude", "api_key", FAILOVER_RATE_LIMIT,
                                              "HTTP 429", 1000) == 1);
   assert(delegate_credentials_save_file(path) == 0);

   delegate_credentials_reset_for_test();
   assert(delegate_credentials_load_file(path, 1000) == 2);
   /* The per-principal row reloaded under its own principal, isolated from "". */
   assert(delegate_credentials_cooldown_remaining("uid:alice", "claude", "api_key", 1000) > 0);
   assert(delegate_credentials_cooldown_remaining("", "claude", "api_key", 1000) == 0);
   /* The shared row is visible to the operator snapshot; the vault row is not. */
   delegate_credential_snapshot_t rows[8];
   int n = delegate_credentials_snapshot(NULL, rows, 8);
   assert(n == 1);
   assert(strcmp(rows[0].agent_name, "openrouter") == 0 && strcmp(rows[0].cred_name, "main") == 0);
   unlink(path);

   /* Legacy v1 file (no principal column) loads into the shared "" pool. */
   delegate_credentials_reset_for_test();
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs("aimee-delegate-credential-state-v1\nopenrouter\tmain\t1\t1120\n", fp);
   fclose(fp);
   assert(delegate_credentials_load_file(path, 1000) == 1);
   assert(delegate_credentials_cooldown_remaining("", "openrouter", "main", 1000) == 120);
   unlink(path);
   printf("  PASS: test_save_load_v2_roundtrips_principal\n");
}

int main(void)
{
   printf("delegate_credentials:\n");
   test_acquire_round_robin();
   test_release_makes_credential_available();
   test_cooldown_skips_credential();
   test_capture_headers_records_quota_windows();
   test_acquire_prefers_freshest_quota();
   test_report_failure_marks_and_rotates();
   test_rotate_after_failure_releases_and_leases_next();
   test_rotate_after_non_pool_failure_keeps_lease();
   test_billing_and_auth_failures_are_not_reacquired();
   test_health_state_persists_across_reset();
   test_isolation_between_agents();
   test_lease_state_grows_beyond_initial_capacity();
   test_empty_pool_returns_error();
   test_is_rate_limit_classifier();
   test_failure_classifier_maps_pool_reasons();
   test_principal_cooldown_isolation();
   test_cooldown_remaining_reports_seconds();
   test_save_load_v2_roundtrips_principal();
   printf("ok\n");
   return 0;
}
