/* On-demand P7 D2b integration harness.  Each command is a fresh process so
 * TPM/provider caches and the process-local primary epoch cannot leak between
 * crash-boundary fixtures.  Driven only by p7_reseal_d2b_swtpm_pg_test.sh. */
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/org_vault_rewrap.h"
#include "kb/kb_vault_policy.h"
#include "modules/vault/vault_crypto.h"
#include "modules/vault/vault_custody_tpm2.h"
#include "modules/vault/vault_kek_check.h"
#include "modules/vault/vault_reseal_orchestrator.h"
#include "modules/vault/vault_reseal_receipt.h"
#include "modules/vault/vault_server_key.h"

#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECRET_ROWS 257
#define CHECK_ROWS  5
#define ERR_CAP     512

static const char *const actor = "p7-d2b-ct260";
static const char *const request_id = "p7-d2b-ct260-request";
static const char *const principals[CHECK_ROWS] = {
    "p7-d2b-main", "p7-d2b-empty", "p7-d2b-check-only", "p7-d2b-delim|:name", "p7-d2b-utf8-Î¼"};

static void fail(const char *what)
{
   fprintf(stderr, "p7-d2b-live: FAIL: %s\n", what);
   exit(EXIT_FAILURE);
}

#define CHECK(x)                                                                                   \
   do                                                                                              \
   {                                                                                               \
      if (!(x))                                                                                    \
         fail(#x);                                                                                 \
   } while (0)

static const char *db_url(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
      fail("AIMEE_TEST_PG_URL is required");
   return url;
}

static void pg_fail(const char *what, const char *err)
{
   fprintf(stderr, "p7-d2b-live: FAIL: %s: %s\n", what, err ? err : "postgres error");
   exit(EXIT_FAILURE);
}

static void sql_exec(const char *sql)
{
   char err[ERR_CAP] = "";
   if (aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) != 0)
      pg_fail(sql, err);
}

static int64_t sql_i64(const char *sql)
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st || aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      pg_fail(sql, err);
   int64_t value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static void db_open(void)
{
   db2_set_pool_size(4);
   if (db2_init(db_url()) != 0)
      fail("db2_init");
}

static int hex_key(const char *hex, uint8_t key[VAULT_KEK_LEN])
{
   if (!hex || strlen(hex) != VAULT_KEK_LEN * 2u)
      return -1;
   for (size_t i = 0; i < VAULT_KEK_LEN; i++)
   {
      unsigned int b = 0;
      if (sscanf(hex + i * 2, "%2x", &b) != 1)
         return -1;
      key[i] = (uint8_t)b;
   }
   return 0;
}

static int hex_op(const char *hex, uint8_t op[VAULT_RESEAL_OPERATION_ID_LEN])
{
   return vault_reseal_operation_id_from_hex(hex, op);
}

static int bind_tpm(void)
{
   char err[192] = "";
   if (kb_vault_policy_select("tpm2", err, sizeof(err)) != 0)
   {
      fprintf(stderr, "p7-d2b-live: TPM bind: %s\n", err);
      return -1;
   }
   return 0;
}

static void make_dek(int64_t version, uint8_t dek[VAULT_DEK_LEN])
{
   for (size_t i = 0; i < VAULT_DEK_LEN; i++)
      dek[i] = (uint8_t)((uint64_t)version * 29u + i * 11u + 3u);
}

static void seed_salts(const uint8_t old_kek[VAULT_KEK_LEN])
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "INSERT INTO org_vault_salt(principal,salt,kek_check) VALUES(?1,?2,?3)", err,
       sizeof(err));
   if (!st)
      pg_fail("prepare salt seed", err);
   for (size_t i = 0; i < CHECK_ROWS; i++)
   {
      uint8_t salt[VAULT_SALT_LEN], check[VAULT_WRAPPED_DEK_LEN];
      for (size_t j = 0; j < sizeof(salt); j++)
         salt[j] = (uint8_t)(i * 19u + j + 1u);
      memset(check, 0, sizeof(check));
      int empty = i == 1;
      if (!empty)
         CHECK(vault_kek_check_wrap(old_kek, check) == 0);
      CHECK(aimee_pg_reset(st) == 0);
      CHECK(aimee_pg_bind_text(st, "?1", principals[i]) == 0);
      CHECK(aimee_pg_bind_blob(st, "?2", salt, sizeof(salt)) == 0);
      CHECK(aimee_pg_bind_blob(st, "?3", check, empty ? 0 : sizeof(check)) == 0);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ERR)
         pg_fail("seed salt", err);
      OPENSSL_cleanse(salt, sizeof(salt));
      OPENSSL_cleanse(check, sizeof(check));
   }
   aimee_pg_finalize(st);
}

static void seed_secrets(const uint8_t old_kek[VAULT_KEK_LEN])
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO org_vault_secret(principal,team_id,agent,cred,version,wrapped_dek,nonce,"
       "ciphertext,tag) VALUES(?1,NULL,?2,?3,?4,?5,?6,?7,?8)",
       err, sizeof(err));
   if (!st)
      pg_fail("prepare secret seed", err);
   for (int64_t version = 1; version <= SECRET_ROWS; version++)
   {
      uint8_t dek[VAULT_DEK_LEN], wrapped[VAULT_WRAPPED_DEK_LEN];
      uint8_t nonce[VAULT_GCM_NONCE_LEN], ciphertext[24], tag[VAULT_GCM_TAG_LEN];
      make_dek(version, dek);
      CHECK(vault_dek_wrap(old_kek, dek, wrapped) == 0);
      for (size_t i = 0; i < sizeof(nonce); i++)
         nonce[i] = (uint8_t)(version + (int64_t)i);
      for (size_t i = 0; i < sizeof(ciphertext); i++)
         ciphertext[i] = (uint8_t)(version * 3 + (int64_t)i);
      for (size_t i = 0; i < sizeof(tag); i++)
         tag[i] = (uint8_t)(version * 5 + (int64_t)i);
      CHECK(aimee_pg_reset(st) == 0);
      CHECK(aimee_pg_bind_text(st, "?1", principals[0]) == 0);
      CHECK(aimee_pg_bind_text(st, "?2", version & 1 ? "bedrock" : "agent|delim") == 0);
      CHECK(aimee_pg_bind_text(st, "?3", version & 1 ? "primary" : "cred:name") == 0);
      CHECK(aimee_pg_bind_int64(st, "?4", version) == 0);
      CHECK(aimee_pg_bind_blob(st, "?5", wrapped, sizeof(wrapped)) == 0);
      CHECK(aimee_pg_bind_blob(st, "?6", nonce, sizeof(nonce)) == 0);
      CHECK(aimee_pg_bind_blob(st, "?7", ciphertext, sizeof(ciphertext)) == 0);
      CHECK(aimee_pg_bind_blob(st, "?8", tag, sizeof(tag)) == 0);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ERR)
         pg_fail("seed secret", err);
      OPENSSL_cleanse(dek, sizeof(dek));
      OPENSSL_cleanse(wrapped, sizeof(wrapped));
   }
   aimee_pg_finalize(st);
   sql_exec("INSERT INTO org_vault_current(principal,agent,cred,version) "
            "VALUES('p7-d2b-main','bedrock','primary',257)");
}

static void seed(const char *old_hex)
{
   uint8_t old_kek[VAULT_KEK_LEN];
   CHECK(hex_key(old_hex, old_kek) == 0);
   db_open();
   CHECK(sql_i64("SELECT count(*) FROM org_vault_secret") == 0);
   CHECK(sql_i64("SELECT count(*) FROM kb_vault_rewrap_operation") == 0);
   seed_salts(old_kek);
   seed_secrets(old_kek);
   CHECK(sql_i64("SELECT count(*) FROM org_vault_secret") == SECRET_ROWS);
   OPENSSL_cleanse(old_kek, sizeof(old_kek));
   db2_shutdown();
   puts("p7-d2b-live: seeded 257 secrets and 5 checks");
}

static void provision(const char *old_hex, const char *secret)
{
   uint8_t old_kek[VAULT_KEK_LEN];
   char err[256] = "";
   CHECK(hex_key(old_hex, old_kek) == 0);
   CHECK(vault_custody_tpm2_provision(old_kek, secret, err, sizeof(err)) == 0);
   OPENSSL_cleanse(old_kek, sizeof(old_kek));
   puts("p7-d2b-live: provisioned initial TPM KEK");
}

static void fixture_begin(const char *op_hex, const char *secret, const char *new_hex, int prepare)
{
   uint8_t op[16], new_kek[VAULT_KEK_LEN];
   CHECK(hex_op(op_hex, op) == 0);
   if (prepare)
      CHECK(hex_key(new_hex, new_kek) == 0);
   CHECK(bind_tpm() == 0);
   uint64_t generation = 0;
   CHECK(vault_custody_tpm2_nv_generation(secret, &generation) == VAULT_TPM2_RESEAL_OK);
   CHECK(generation < (uint64_t)INT64_MAX);
   db_open();
   db2_vault_rewrap_tx_t *tx = NULL;
   int64_t epoch = 0, fence = 0;
   db2_vault_rewrap_state_t state = DB2_VAULT_REWRAP_PREPARING;
   CHECK(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   CHECK(db2_vault_rewrap_begin(tx, actor, request_id, op, (int64_t)generation,
                                (int64_t)generation + 1, &epoch, &fence,
                                &state) == DB2_VAULT_REWRAP_OK);
   CHECK(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && tx == NULL);
   if (prepare)
   {
      vault_tpm2_reseal_receipt_t receipt;
      uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
      memset(&receipt, 0, sizeof(receipt));
      CHECK(vault_custody_tpm2_reseal_prepare(op, generation, new_kek, secret, &receipt) ==
            VAULT_TPM2_RESEAL_OK);
      CHECK(vault_reseal_receipt_encode(&receipt, wire) == 0);
      CHECK(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
      CHECK(db2_vault_rewrap_record_prepared(tx, op, fence, (int64_t)generation,
                                             (int64_t)generation + 1, wire) == DB2_VAULT_REWRAP_OK);
      CHECK(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && tx == NULL);
      OPENSSL_cleanse(&receipt, sizeof(receipt));
      OPENSSL_cleanse(wire, sizeof(wire));
      OPENSSL_cleanse(new_kek, sizeof(new_kek));
   }
   db2_shutdown();
   OPENSSL_cleanse(op, sizeof(op));
   puts(prepare ? "p7-d2b-live: custody_prepared fixture" : "p7-d2b-live: preparing fixture");
}

static vault_reseal_orchestrator_result_t parse_result(const char *s)
{
   if (strcmp(s, "completed") == 0)
      return VAULT_RESEAL_ORCHESTRATOR_COMPLETED;
   if (strcmp(s, "safe_retry") == 0)
      return VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
   if (strcmp(s, "recovery_required") == 0)
      return VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED;
   if (strcmp(s, "integrity") == 0)
      return VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
   fail("unknown expected result");
   return VAULT_RESEAL_ORCHESTRATOR_ERROR;
}

static void run_orchestrator(const char *mode, const char *op_hex, const char *secret,
                             const char *expected)
{
   uint8_t op[16];
   char actor_buf[576], request_buf[201];
   snprintf(actor_buf, sizeof(actor_buf), "%s", actor);
   snprintf(request_buf, sizeof(request_buf), "%s", request_id);
   CHECK(hex_op(op_hex, op) == 0);
   db_open();
   int64_t epoch = sql_i64("SELECT seal_epoch FROM kb_vault_control WHERE singleton=1");
   CHECK(bind_tpm() == 0);
   CHECK(vault_primary_epoch_initialize((uint64_t)epoch) == VAULT_MAINTENANCE_OK);
   vault_reseal_orchestrator_request_t req = {
       .mode = strcmp(mode, "start") == 0 ? VAULT_RESEAL_ORCHESTRATOR_START
                                          : VAULT_RESEAL_ORCHESTRATOR_RESUME,
       .actor = actor_buf,
       .request_id = request_buf,
       .provider_secret = (const uint8_t *)secret,
       .provider_secret_len = strlen(secret),
   };
   memcpy(req.operation_id, op, sizeof(op));
   vault_reseal_orchestrator_output_t out;
   vault_reseal_orchestrator_result_t got =
       vault_reseal_orchestrator_run(&req, &vault_reseal_orchestrator_default_deps, &out);
   vault_reseal_orchestrator_result_t want = parse_result(expected);
   if (got != want)
   {
      fprintf(stderr, "p7-d2b-live: result got=%d want=%d state=%d failure=%s\n", got, want,
              out.has_state ? (int)out.state : -1, out.failure_class);
      fail("orchestrator result");
   }
   db2_shutdown();
   OPENSSL_cleanse(&req, sizeof(req));
   OPENSSL_cleanse(&out, sizeof(out));
   OPENSSL_cleanse(op, sizeof(op));
   printf("p7-d2b-live: orchestrator %s -> %s\n", mode, expected);
}

static void assert_state(const char *state, int64_t operations)
{
   db_open();
   CHECK(sql_i64("SELECT count(*) FROM kb_vault_rewrap_operation") == operations);
   if (operations)
   {
      char query[256];
      snprintf(query, sizeof(query),
               "SELECT count(*) FROM kb_vault_rewrap_operation WHERE state='%s'", state);
      CHECK(sql_i64(query) == operations);
      CHECK(sql_i64("SELECT sealed::int FROM kb_vault_control WHERE singleton=1") == 1);
   }
   db2_shutdown();
   puts("p7-d2b-live: durable state asserted");
}

static void verify_completed(const char *old_hex, const char *secret)
{
   uint8_t old_kek[VAULT_KEK_LEN], active_kek[VAULT_KEK_LEN], dek[VAULT_DEK_LEN];
   CHECK(hex_key(old_hex, old_kek) == 0);
   db_open();
   CHECK(sql_i64("SELECT count(*) FROM kb_vault_rewrap_operation WHERE state='completed'") == 1);
   CHECK(sql_i64("SELECT sealed::int FROM kb_vault_control WHERE singleton=1") == 1);
   CHECK(sql_i64("SELECT count(*) FROM org_vault_secret") == SECRET_ROWS);
   CHECK(sql_i64("SELECT count(*) FROM kb_vault_rewrap_dek_stage") == SECRET_ROWS);
   CHECK(bind_tpm() == 0);
   CHECK(vault_unseal(secret, strlen(secret)) == 0);
   CHECK(vault_server_kek(active_kek) == 0);

   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "SELECT version,wrapped_dek,nonce,ciphertext,tag FROM org_vault_secret ORDER BY id", err,
       sizeof(err));
   if (!st)
      pg_fail("verify secret prepare", err);
   int64_t seen = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int64_t version = aimee_pg_column_int64(st, 0);
      const uint8_t *wrapped = aimee_pg_column_blob(st, 1);
      CHECK(wrapped && aimee_pg_column_bytes(st, 1) == VAULT_WRAPPED_DEK_LEN);
      uint8_t expected[VAULT_DEK_LEN];
      make_dek(version, expected);
      memset(dek, 0xa5, sizeof(dek));
      CHECK(vault_dek_unwrap(old_kek, wrapped, dek) != 0);
      CHECK(vault_dek_unwrap(active_kek, wrapped, dek) == 0);
      CHECK(CRYPTO_memcmp(dek, expected, sizeof(dek)) == 0);
      uint8_t nonce[VAULT_GCM_NONCE_LEN], cipher[24], tag[VAULT_GCM_TAG_LEN];
      const uint8_t *column = aimee_pg_column_blob(st, 2);
      CHECK(column && aimee_pg_column_bytes(st, 2) == VAULT_GCM_NONCE_LEN);
      memcpy(nonce, column, sizeof(nonce));
      column = aimee_pg_column_blob(st, 3);
      CHECK(column && aimee_pg_column_bytes(st, 3) == 24);
      memcpy(cipher, column, sizeof(cipher));
      column = aimee_pg_column_blob(st, 4);
      CHECK(column && aimee_pg_column_bytes(st, 4) == VAULT_GCM_TAG_LEN);
      memcpy(tag, column, sizeof(tag));
      for (size_t i = 0; i < VAULT_GCM_NONCE_LEN; i++)
         CHECK(nonce[i] == (uint8_t)(version + (int64_t)i));
      for (size_t i = 0; i < 24; i++)
         CHECK(cipher[i] == (uint8_t)(version * 3 + (int64_t)i));
      for (size_t i = 0; i < VAULT_GCM_TAG_LEN; i++)
         CHECK(tag[i] == (uint8_t)(version * 5 + (int64_t)i));
      OPENSSL_cleanse(expected, sizeof(expected));
      OPENSSL_cleanse(dek, sizeof(dek));
      seen++;
   }
   aimee_pg_finalize(st);
   CHECK(seen == SECRET_ROWS);

   st = aimee_pg_prepare(db2_conn(), "SELECT kek_check FROM org_vault_salt ORDER BY principal", err,
                         sizeof(err));
   if (!st)
      pg_fail("verify check prepare", err);
   int empty = 0, nonempty = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int n = aimee_pg_column_bytes(st, 0);
      const uint8_t *check = aimee_pg_column_blob(st, 0);
      if (n == 0)
         empty++;
      else
      {
         CHECK(check && n == VAULT_WRAPPED_DEK_LEN);
         CHECK(vault_kek_check_verify(old_kek, check) != 0);
         CHECK(vault_kek_check_verify(active_kek, check) == 0);
         nonempty++;
      }
   }
   aimee_pg_finalize(st);
   CHECK(empty == 1 && nonempty == CHECK_ROWS - 1);
   CHECK(vault_seal() == 0);
   OPENSSL_cleanse(old_kek, sizeof(old_kek));
   OPENSSL_cleanse(active_kek, sizeof(active_kek));
   OPENSSL_cleanse(dek, sizeof(dek));
   db2_shutdown();
   puts("p7-d2b-live: completed inventory verified under installed KEK only");
}

static void unseal_refused(const char *secret)
{
   CHECK(bind_tpm() == 0);
   CHECK(vault_unseal(secret, strlen(secret)) != 0);
   CHECK(vault_is_sealed() == 1);
   puts("p7-d2b-live: stale blob refused");
}

static void unseal_ok(const char *secret)
{
   uint8_t kek[VAULT_KEK_LEN];
   CHECK(bind_tpm() == 0);
   CHECK(vault_unseal(secret, strlen(secret)) == 0);
   CHECK(vault_server_kek(kek) == 0);
   OPENSSL_cleanse(kek, sizeof(kek));
   CHECK(vault_seal() == 0);
   puts("p7-d2b-live: installed blob unsealed");
}

static void diagnose_verify(const char *op_hex, const char *secret)
{
   uint8_t op[16], kek[VAULT_KEK_LEN], dek[VAULT_DEK_LEN];
   db2_vault_rewrap_snapshot_t snap;
   db2_vault_rewrap_verify_summary_t sum;
   db2_vault_rewrap_secret_t secrets[DB2_VAULT_REWRAP_PAGE_MAX];
   db2_vault_rewrap_check_t checks[DB2_VAULT_REWRAP_PAGE_MAX];
   db2_vault_rewrap_cursor_t cursor = {{0}, 0}, next = {{0}, 0};
   db2_vault_rewrap_tx_t *tx = NULL;
   CHECK(hex_op(op_hex, op) == 0);
   db_open();
   CHECK(db2_vault_rewrap_snapshot(op, &snap) == DB2_VAULT_REWRAP_OK);
   CHECK(bind_tpm() == 0);
   CHECK(vault_unseal(secret, strlen(secret)) == 0);
   CHECK(vault_server_kek(kek) == 0);
   CHECK(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   CHECK(db2_vault_rewrap_verify_summary(tx, op, snap.fencing_token, &sum) == DB2_VAULT_REWRAP_OK);
   int64_t after = 0, seen_s = 0, seen_c = 0;
   while (seen_s < sum.secret_count)
   {
      size_t n = 0;
      int limit = sum.secret_count - seen_s > DB2_VAULT_REWRAP_PAGE_MAX
                      ? DB2_VAULT_REWRAP_PAGE_MAX
                      : (int)(sum.secret_count - seen_s);
      db2_vault_rewrap_result_t rc = db2_vault_rewrap_verify_secret_page(
          tx, op, snap.fencing_token, after, limit, secrets, DB2_VAULT_REWRAP_PAGE_MAX, &n);
      if (rc != DB2_VAULT_REWRAP_OK)
      {
         fprintf(stderr, "p7-d2b-live: diagnose secret page rc=%d seen=%lld limit=%d\n", rc,
                 (long long)seen_s, limit);
         fail("diagnose secret page");
      }
      for (size_t i = 0; i < n; i++)
      {
         CHECK(vault_dek_unwrap(kek, secrets[i].wrapped_dek, dek) == 0);
         after = secrets[i].source_id;
      }
      seen_s += (int64_t)n;
   }
   size_t n = 0;
   CHECK(db2_vault_rewrap_verify_secret_page(tx, op, snap.fencing_token, after, 1, secrets,
                                             DB2_VAULT_REWRAP_PAGE_MAX,
                                             &n) == DB2_VAULT_REWRAP_OK &&
         n == 0);
   while (seen_c < sum.check_count)
   {
      int limit = sum.check_count - seen_c > DB2_VAULT_REWRAP_PAGE_MAX
                      ? DB2_VAULT_REWRAP_PAGE_MAX
                      : (int)(sum.check_count - seen_c);
      CHECK(db2_vault_rewrap_verify_check_page(tx, op, snap.fencing_token, &cursor, limit, checks,
                                               DB2_VAULT_REWRAP_PAGE_MAX, &n,
                                               &next) == DB2_VAULT_REWRAP_OK);
      for (size_t i = 0; i < n; i++)
         CHECK(!checks[i].kek_check_len || vault_kek_check_verify(kek, checks[i].kek_check) == 0);
      seen_c += (int64_t)n;
      cursor = next;
   }
   CHECK(db2_vault_rewrap_verify_check_page(tx, op, snap.fencing_token, &cursor, 1, checks,
                                            DB2_VAULT_REWRAP_PAGE_MAX, &n,
                                            &next) == DB2_VAULT_REWRAP_OK &&
         n == 0);
   CHECK(db2_vault_rewrap_verify_crypto_ack(tx, op, snap.fencing_token) == DB2_VAULT_REWRAP_OK);
   db2_vault_rewrap_tx_rollback(&tx);
   CHECK(vault_seal() == 0);
   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(dek, sizeof(dek));
   db2_vault_rewrap_snapshot_clear(&snap);
   db2_vault_rewrap_verify_summary_clear(&sum);
   db2_shutdown();
   puts("p7-d2b-live: typed verification diagnosis passed");
}

static void usage(void)
{
   fputs("usage: p7-reseal-d2b-live provision OLD SECRET | seed OLD | "
         "fixture-preparing OP SECRET | fixture-prepared OP SECRET NEW | "
         "run start|resume OP SECRET EXPECT | assert-state STATE COUNT | "
         "verify OLD SECRET | diagnose OP SECRET | unseal-ok SECRET | unseal-refused SECRET\n",
         stderr);
   exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
   if (argc == 4 && strcmp(argv[1], "provision") == 0)
      provision(argv[2], argv[3]);
   else if (argc == 3 && strcmp(argv[1], "seed") == 0)
      seed(argv[2]);
   else if (argc == 4 && strcmp(argv[1], "fixture-preparing") == 0)
      fixture_begin(argv[2], argv[3], NULL, 0);
   else if (argc == 5 && strcmp(argv[1], "fixture-prepared") == 0)
      fixture_begin(argv[2], argv[3], argv[4], 1);
   else if (argc == 6 && strcmp(argv[1], "run") == 0)
      run_orchestrator(argv[2], argv[3], argv[4], argv[5]);
   else if (argc == 4 && strcmp(argv[1], "assert-state") == 0)
      assert_state(argv[2], strtoll(argv[3], NULL, 10));
   else if (argc == 4 && strcmp(argv[1], "verify") == 0)
      verify_completed(argv[2], argv[3]);
   else if (argc == 3 && strcmp(argv[1], "unseal-refused") == 0)
      unseal_refused(argv[2]);
   else if (argc == 3 && strcmp(argv[1], "unseal-ok") == 0)
      unseal_ok(argv[2]);
   else if (argc == 4 && strcmp(argv[1], "diagnose") == 0)
      diagnose_verify(argv[2], argv[3]);
   else
      usage();
   return 0;
}
