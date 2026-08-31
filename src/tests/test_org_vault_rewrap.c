#include "modules/db2/c/db2_pool.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/org_vault_rewrap.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aimee_pg_stmt
{
   int step;
   int query;
};

enum
{
   QUERY_STATE,
   QUERY_BEGIN,
   QUERY_SNAPSHOT,
   QUERY_VERIFY_SUMMARY,
   QUERY_VERIFY_SECRET,
   QUERY_VERIFY_CHECK
};

static int g_conn, g_returns, g_discards;
static const char *g_fail_command;
static const char *g_fail_state;
static int g_step_fail_at;
static const char *g_stmt_state = "XX000";
static aimee_pg_prepare_error_t g_prepare_failure = AIMEE_PG_PREPARE_OK;
static int g_empty_rows;
static const char *g_returned_op = "01000000000000000000000000000000";
static const char *g_state_response = "promoted";
static const char *g_check_principal = "a";
static int64_t g_summary_secrets, g_summary_checks, g_secret_id = 1;
static int g_verify_page_rows;

static int invalid_operation_id_to_hex(const uint8_t operation_id[16], char out[33])
{
   (void)operation_id;
   memset(out, 'A', 32);
   out[32] = '\0';
   return 0;
}

static void test_reseal_contract(void)
{
   uint8_t operation_id[16] = {1}, decoded_id[16];
   char hex[33];
   aimee_db2_register_vault_reseal_provider(NULL);
   memset(hex, 'x', sizeof(hex));
   assert(db2_vault_reseal_operation_id_to_hex(operation_id, hex) == -1);
   assert(hex[0] == '\0');

   db2_vault_reseal_provider_t invalid = {.operation_id_to_hex = invalid_operation_id_to_hex};
   aimee_db2_register_vault_reseal_provider(&invalid);
   assert(db2_vault_reseal_operation_id_to_hex(operation_id, hex) == -1);

   const db2_vault_reseal_provider_t provider = {
       .operation_id_to_hex = vault_reseal_operation_id_to_hex,
       .operation_id_from_hex = vault_reseal_operation_id_from_hex,
       .receipt_decode = vault_reseal_receipt_decode,
       .receipt_digest = vault_reseal_receipt_digest,
   };
   aimee_db2_register_vault_reseal_provider(&provider);
   assert(db2_vault_reseal_operation_id_to_hex(operation_id, hex) == 0);
   assert(db2_vault_reseal_operation_id_from_hex(hex, decoded_id) == 0);
   assert(memcmp(operation_id, decoded_id, sizeof(operation_id)) == 0);

   vault_tpm2_reseal_receipt_t receipt = {.old_generation = 7, .new_generation = 8};
   memcpy(receipt.operation_id, operation_id, sizeof(operation_id));
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN], digest[32];
   vault_tpm2_reseal_receipt_t decoded;
   assert(vault_reseal_receipt_encode(&receipt, wire) == 0);
   assert(db2_vault_reseal_receipt_decode(wire, sizeof(wire), &decoded) == 0);
   assert(memcmp(decoded.operation_id, operation_id, sizeof(operation_id)) == 0);
   assert(db2_vault_reseal_receipt_digest(wire, digest) == 0);
}

int db2_pool_active(void)
{
   return 1;
}
void *db2_pool_lease(int timeout_ms)
{
   (void)timeout_ms;
   return &g_conn;
}
void db2_pool_return(void *conn)
{
   assert(conn == &g_conn);
   g_returns++;
}
void db2_pool_discard(void *conn)
{
   assert(conn == &g_conn);
   g_discards++;
}

int aimee_pg_exec_sqlstate(void *conn, const char *sql, char state[6], char *err, size_t errlen)
{
   (void)conn;
   (void)err;
   (void)errlen;
   state[0] = '\0';
   if (g_fail_command && strstr(sql, g_fail_command))
   {
      snprintf(state, 6, "%s", g_fail_state);
      return -1;
   }
   return 0;
}

static struct aimee_pg_stmt g_stmt;

aimee_pg_stmt_t *aimee_pg_prepare_ex(void *c, const char *s, aimee_pg_prepare_error_t *kind,
                                     char *e, size_t n)
{
   (void)c;
   (void)e;
   (void)n;
   if (g_prepare_failure != AIMEE_PG_PREPARE_OK)
   {
      if (kind)
         *kind = g_prepare_failure;
      return NULL;
   }
   g_stmt.step = 0;
   g_stmt.query = strstr(s, "org_vault_rewrap_begin")
                      ? QUERY_BEGIN
                      : (strstr(s, "org_vault_rewrap_snapshot")
                             ? QUERY_SNAPSHOT
                             : (strstr(s, "org_vault_rewrap_verify_summary")
                                    ? QUERY_VERIFY_SUMMARY
                                    : (strstr(s, "org_vault_rewrap_verify_secret_page")
                                           ? QUERY_VERIFY_SECRET
                                           : (strstr(s, "org_vault_rewrap_verify_check_page")
                                                  ? QUERY_VERIFY_CHECK
                                                  : QUERY_STATE))));
   if (kind)
      *kind = AIMEE_PG_PREPARE_OK;
   return &g_stmt;
}
aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *s, char *e, size_t n)
{
   return aimee_pg_prepare_ex(c, s, NULL, e, n);
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   (void)s;
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *e, size_t n)
{
   (void)e;
   (void)n;
   if (g_step_fail_at && s->step + 1 == g_step_fail_at)
   {
      s->step++;
      return AIMEE_PG_ERR;
   }
   if (s->query == QUERY_VERIFY_SECRET || s->query == QUERY_VERIFY_CHECK)
      return s->step++ < g_verify_page_rows ? AIMEE_PG_ROW : AIMEE_PG_DONE;
   if (g_empty_rows && s->step++ == 0)
      return AIMEE_PG_DONE;
   return s->step++ == 0 ? AIMEE_PG_ROW : AIMEE_PG_DONE;
}
const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *s)
{
   (void)s;
   return g_stmt_state;
}
int aimee_pg_bind_int(aimee_pg_stmt_t *s, const char *n, int v)
{
   (void)s;
   (void)n;
   (void)v;
   return 0;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *n, int64_t v)
{
   (void)s;
   (void)n;
   (void)v;
   return 0;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *n, const char *v)
{
   (void)s;
   (void)n;
   (void)v;
   return 0;
}
int aimee_pg_bind_blob(aimee_pg_stmt_t *s, const char *n, const void *v, int z)
{
   (void)s;
   (void)n;
   (void)v;
   (void)z;
   return 0;
}
int aimee_pg_column_is_null(aimee_pg_stmt_t *s, int c)
{
   if (s->query == QUERY_SNAPSHOT)
      return !(c == 2 || c == 3 || c == 4 || c == 5 || c == 8 || c == 9);
   if (s->query == QUERY_VERIFY_SUMMARY || s->query == QUERY_VERIFY_SECRET ||
       s->query == QUERY_VERIFY_CHECK)
      return 0;
   return 1;
}
int aimee_pg_column_bytes(aimee_pg_stmt_t *s, int c)
{
   if (s->query == QUERY_VERIFY_SUMMARY)
      return c >= 2 && c <= 4 ? 32 : 0;
   if (s->query == QUERY_VERIFY_SECRET)
      return c == 5 ? VAULT_WRAPPED_DEK_LEN : 0;
   if (s->query == QUERY_VERIFY_CHECK)
      return c == 2 ? (int)strlen(g_check_principal) : 0;
   return 0;
}
const void *aimee_pg_column_blob(aimee_pg_stmt_t *s, int c)
{
   static const uint8_t zero[VAULT_WRAPPED_DEK_LEN] = {0};
   if (s->query == QUERY_VERIFY_SUMMARY || (s->query == QUERY_VERIFY_SECRET && c == 5))
      return zero;
   if (s->query == QUERY_VERIFY_CHECK)
      return c == 2 ? (const void *)g_check_principal : (const void *)zero;
   return NULL;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int c)
{
   if (s->query == QUERY_BEGIN)
      return c == 0 ? g_returned_op : "preparing";
   if (s->query == QUERY_SNAPSHOT)
      return c == 0 ? g_returned_op : "preparing";
   if (s->query == QUERY_VERIFY_SECRET)
      return c == 1 ? "principal" : (c == 2 ? "agent" : "cred");
   if (s->query == QUERY_VERIFY_CHECK)
      return g_check_principal;
   return g_state_response;
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int c)
{
   if (s->query == QUERY_SNAPSHOT)
      return c == 5 ? 1 : (c == 2 || c == 3 ? 1 : 0);
   if (s->query == QUERY_VERIFY_SUMMARY)
      return c == 0 ? g_summary_secrets : g_summary_checks;
   if (s->query == QUERY_VERIFY_SECRET)
      return c == 0 ? g_secret_id : 1;
   return 1;
}

static int all_zero(const void *ptr, size_t len)
{
   const unsigned char *p = ptr;
   unsigned char any = 0;
   for (size_t i = 0; i < len; i++)
      any |= p[i];
   return any == 0;
}

static int all_byte(const void *ptr, size_t len, unsigned char value)
{
   const unsigned char *p = ptr;
   for (size_t i = 0; i < len; i++)
      if (p[i] != value)
         return 0;
   return 1;
}

static void test_sqlstate_mapping(void)
{
   assert(db2_vault_rewrap_classify_sqlstate("P7C01") == DB2_VAULT_REWRAP_CONFLICT);
   assert(db2_vault_rewrap_classify_sqlstate("P7I01") == DB2_VAULT_REWRAP_INTEGRITY);
   assert(db2_vault_rewrap_classify_sqlstate("40001") == DB2_VAULT_REWRAP_TRANSIENT);
   assert(db2_vault_rewrap_classify_sqlstate("40P01") == DB2_VAULT_REWRAP_TRANSIENT);
   assert(db2_vault_rewrap_classify_sqlstate("55000") == DB2_VAULT_REWRAP_BUSY);
   assert(db2_vault_rewrap_classify_sqlstate(NULL) == DB2_VAULT_REWRAP_ERROR);
}

static void *wrong_thread_commit(void *p)
{
   db2_vault_rewrap_tx_t **tx = p;
   assert(db2_vault_rewrap_tx_commit(tx) == DB2_VAULT_REWRAP_INVALID);
   return NULL;
}

static void test_public_handle_phases(void)
{
   db2_vault_rewrap_tx_t *tx = NULL;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   pthread_t other;
   assert(pthread_create(&other, NULL, wrong_thread_commit, &tx) == 0);
   assert(pthread_join(other, NULL) == 0);
   assert(tx != NULL);

   db2_vault_rewrap_verify_summary_t summary;
   assert(db2_vault_rewrap_verify_summary(tx, NULL, 1, &summary) == DB2_VAULT_REWRAP_INVALID);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID);
   assert(tx != NULL);
   db2_vault_rewrap_tx_rollback(&tx);
   assert(tx == NULL && g_returns == 1);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   g_fail_command = "COMMIT";
   g_fail_state = "40001";
   assert(db2_vault_rewrap_mark_committing(tx, (const uint8_t[16]){1}, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_TRANSIENT);
   assert(tx == NULL && g_discards == 1);

   g_fail_state = "P7C01";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_mark_committing(tx, (const uint8_t[16]){1}, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_TRANSIENT);
   assert(tx == NULL && g_discards == 2);

   g_fail_command = "ROLLBACK";
   g_fail_state = "08006";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   db2_vault_rewrap_tx_rollback(&tx);
   assert(tx == NULL && g_discards == 3);
}

static void test_transaction_kind_gates(void)
{
   uint8_t op[16] = {1};
   db2_vault_rewrap_tx_t *tx = NULL;
   g_fail_command = NULL;

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_promote(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_INVALID);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   db2_vault_rewrap_secret_t source = {.source_id = 1, .version = 1};
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN] = {0};
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_stage_dek(tx, op, 1, &source, wrapped) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_stage_finish(tx, op, 1, &(db2_vault_rewrap_inventory_summary_t){0}) ==
          DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_tx_rollback(&tx);

   db2_vault_rewrap_secret_t secret;
   db2_vault_rewrap_check_t check;
   db2_vault_rewrap_cursor_t cursor = {0}, next = {0};
   size_t count = 0;
   g_empty_rows = 1;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_check_page(tx, op, 1, &cursor, 1, &check, 1, &count, &next) ==
          DB2_VAULT_REWRAP_OK);
   g_empty_rows = 0;
   g_state_response = "wraps_staged";
   assert(db2_vault_rewrap_stage_finish(tx, op, 1, &(db2_vault_rewrap_inventory_summary_t){0}) ==
          DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);
   g_state_response = "promoted";
}

static void test_prepare_and_followup_failures(void)
{
   uint8_t op[16] = {1};
   db2_vault_rewrap_tx_t *tx = NULL;
   g_fail_command = NULL;

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   g_prepare_failure = AIMEE_PG_PREPARE_RESOURCE;
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_ERROR);
   g_prepare_failure = AIMEE_PG_PREPARE_OK;
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   g_step_fail_at = 2;
   g_stmt_state = "P7C01";
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_CONFLICT);
   g_step_fail_at = 0;
   g_stmt_state = "XX000";
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);
}

static void test_receipt_identity_binding(void)
{
   uint8_t op[16] = {1}, other_op[16] = {2};
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
   vault_tpm2_reseal_receipt_t receipt;
   memset(&receipt, 0, sizeof(receipt));
   memcpy(receipt.operation_id, op, sizeof(op));
   receipt.old_generation = 7;
   receipt.new_generation = 8;
   assert(vault_reseal_receipt_encode(&receipt, wire) == 0);

   db2_vault_rewrap_tx_t *tx = NULL;
   /* Exact length alone is insufficient: the typed persistence seam must reject
    * a same-length receipt whose canonical AIMRSEAL header is corrupt. */
   wire[0] ^= 0x80;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_record_prepared(tx, op, 1, 7, 8, wire) == DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_tx_rollback(&tx);
   wire[0] ^= 0x80;

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_record_prepared(tx, other_op, 1, 7, 8, wire) ==
          DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_record_prepared(tx, op, 1, 6, 7, wire) == DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_record_prepared(tx, op, 1, 7, 8, wire) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);
}

static void test_page_cap_rejection(void)
{
   uint8_t op[16] = {1};
   const size_t oversized = DB2_VAULT_REWRAP_PAGE_MAX + 1;
   size_t count = 99;
   db2_vault_rewrap_secret_t *secrets = malloc(oversized * sizeof(*secrets));
   db2_vault_rewrap_check_t *checks = malloc(oversized * sizeof(*checks));
   assert(secrets && checks);
   memset(secrets, 0xa5, oversized * sizeof(*secrets));
   memset(checks, 0xa5, oversized * sizeof(*checks));
   db2_vault_rewrap_tx_t *tx = NULL;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_secret_page(tx, op, 1, 0, 1, secrets, oversized, &count) ==
          DB2_VAULT_REWRAP_INVALID);
   assert(count == 0 && all_zero(secrets, DB2_VAULT_REWRAP_PAGE_MAX * sizeof(*secrets)) &&
          all_byte(&secrets[DB2_VAULT_REWRAP_PAGE_MAX], sizeof(*secrets), 0xa5));
   db2_vault_rewrap_tx_rollback(&tx);

   db2_vault_rewrap_cursor_t cursor = {0}, next;
   memset(&next, 0xa5, sizeof(next));
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_check_page(tx, op, 1, &cursor, 1, checks, oversized, &count,
                                             &next) == DB2_VAULT_REWRAP_INVALID);
   assert(count == 0 && all_zero(checks, DB2_VAULT_REWRAP_PAGE_MAX * sizeof(*checks)) &&
          all_byte(&checks[DB2_VAULT_REWRAP_PAGE_MAX], sizeof(*checks), 0xa5) &&
          all_zero(&next, sizeof(next)));
   db2_vault_rewrap_tx_rollback(&tx);

   memset(secrets, 0xa5, oversized * sizeof(*secrets));
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 0, 1, secrets, oversized, &count) ==
          DB2_VAULT_REWRAP_INVALID);
   assert(count == 0 && all_zero(secrets, DB2_VAULT_REWRAP_PAGE_MAX * sizeof(*secrets)) &&
          all_byte(&secrets[DB2_VAULT_REWRAP_PAGE_MAX], sizeof(*secrets), 0xa5));
   db2_vault_rewrap_tx_rollback(&tx);

   memset(checks, 0xa5, oversized * sizeof(*checks));
   memset(&next, 0xa5, sizeof(next));
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_check_page(tx, op, 1, &cursor, 1, checks, oversized, &count,
                                             &next) == DB2_VAULT_REWRAP_INVALID);
   assert(count == 0 && all_zero(checks, DB2_VAULT_REWRAP_PAGE_MAX * sizeof(*checks)) &&
          all_byte(&checks[DB2_VAULT_REWRAP_PAGE_MAX], sizeof(*checks), 0xa5) &&
          all_zero(&next, sizeof(next)));
   db2_vault_rewrap_tx_rollback(&tx);
   free(checks);
   free(secrets);
}

static void test_operation_id_binding_and_failure_clear(void)
{
   uint8_t op[16] = {1};
   db2_vault_rewrap_tx_t *tx = NULL;
   int64_t epoch = 99, fence = 99;
   db2_vault_rewrap_state_t state = DB2_VAULT_REWRAP_COMPLETED;

   g_returned_op = "02000000000000000000000000000000";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_begin(tx, "actor", "request", op, 0, 1, &epoch, &fence, &state) ==
          DB2_VAULT_REWRAP_INTEGRITY);
   assert(epoch == 0 && fence == 0 && state == 0);
   db2_vault_rewrap_tx_rollback(&tx);

   epoch = fence = 99;
   state = DB2_VAULT_REWRAP_COMPLETED;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_begin(tx, "", "request", op, 0, 1, &epoch, &fence, &state) ==
          DB2_VAULT_REWRAP_INVALID);
   assert(epoch == 0 && fence == 0 && state == 0);
   db2_vault_rewrap_tx_rollback(&tx);

   db2_vault_rewrap_snapshot_t snapshot;
   memset(&snapshot, 0xa5, sizeof(snapshot));
   assert(db2_vault_rewrap_snapshot(op, &snapshot) == DB2_VAULT_REWRAP_INTEGRITY);
   assert(all_zero(&snapshot, sizeof(snapshot)));
   g_returned_op = "01000000000000000000000000000000";

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_begin(tx, "actor", "request", op, 0, 1, &epoch, &fence, &state) ==
          DB2_VAULT_REWRAP_OK);
   assert(epoch == 1 && fence == 1 && state == DB2_VAULT_REWRAP_PREPARING);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);

   assert(db2_vault_rewrap_snapshot(op, &snapshot) == DB2_VAULT_REWRAP_OK);
   assert(memcmp(snapshot.operation_id, op, sizeof(op)) == 0);
   db2_vault_rewrap_snapshot_clear(&snapshot);
}

static void test_page_clearing_and_exhaustion(void)
{
   uint8_t op[16] = {1};
   db2_vault_rewrap_secret_t secret;
   size_t count = 99;
   memset(&secret, 0xa5, sizeof(secret));
   db2_vault_rewrap_tx_t *tx = NULL;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_secret_page(tx, op, 1, -1, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_INVALID);
   assert(count == 0 && all_zero(&secret, sizeof(secret)));
   db2_vault_rewrap_tx_rollback(&tx);

   g_empty_rows = 1;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   assert(count == 0);
   assert(db2_vault_rewrap_source_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_INTEGRITY);
   db2_vault_rewrap_tx_rollback(&tx);

   db2_vault_rewrap_check_t check;
   db2_vault_rewrap_cursor_t cursor = {{0}, 0};
   memset(&check, 0xa5, sizeof(check));
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_check_page(tx, op, 1, &cursor, 1, &check, 1, &count, &cursor) ==
          DB2_VAULT_REWRAP_OK);
   assert(count == 0 && cursor.len == 0 && all_zero(&check, sizeof(check)));
   assert(db2_vault_rewrap_source_check_page(tx, op, 1, &cursor, 1, &check, 1, &count, &cursor) ==
          DB2_VAULT_REWRAP_INTEGRITY);
   db2_vault_rewrap_tx_rollback(&tx);
   g_empty_rows = 0;
}

static void test_ops_vtable_complete(void)
{
   const db2_vault_rewrap_ops_t *o = &db2_vault_rewrap_default_ops;
   assert(o->tx_begin && o->tx_commit && o->tx_rollback && o->snapshot && o->begin &&
          o->record_prepared && o->source_secret_page && o->source_check_page && o->stage_dek &&
          o->stage_check && o->stage_finish && o->mark_committing && o->mark_resealed &&
          o->promote && o->abort && o->recovery_required && o->verify_summary &&
          o->verify_secret_page && o->verify_check_page && o->verify_crypto_ack && o->complete);
}

static void test_authoritative_verification_exhaustion(void)
{
   uint8_t op[16] = {1};
   db2_vault_rewrap_tx_t *tx = NULL;
   db2_vault_rewrap_verify_summary_t summary;
   db2_vault_rewrap_secret_t secret;
   db2_vault_rewrap_check_t check;
   db2_vault_rewrap_cursor_t cursor = {0}, next = {0};
   size_t count = 0;

   g_summary_secrets = g_summary_checks = 1;
   g_verify_page_rows = 0;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_summary(tx, op, 1, &summary) == DB2_VAULT_REWRAP_OK);
   g_verify_page_rows = 1;
   g_secret_id = 1;
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   assert(count == 1);
   assert(db2_vault_rewrap_verify_crypto_ack(tx, op, 1) == DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_summary(tx, op, 1, &summary) == DB2_VAULT_REWRAP_OK);
   g_verify_page_rows = 1;
   g_secret_id = 1;
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   g_verify_page_rows = 0;
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 1, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   assert(count == 0);
   g_verify_page_rows = 1;
   g_check_principal = "a";
   assert(db2_vault_rewrap_verify_check_page(tx, op, 1, &cursor, 1, &check, 1, &count, &next) ==
          DB2_VAULT_REWRAP_OK);
   assert(count == 1 && next.len == 1 && next.bytes[0] == 'a');
   cursor = next;
   g_verify_page_rows = 0;
   assert(db2_vault_rewrap_verify_check_page(tx, op, 1, &cursor, 1, &check, 1, &count, &next) ==
          DB2_VAULT_REWRAP_OK);
   assert(count == 0 && next.len == cursor.len &&
          memcmp(next.bytes, cursor.bytes, cursor.len) == 0);
   assert(db2_vault_rewrap_verify_crypto_ack(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   db2_vault_rewrap_tx_rollback(&tx);

   g_summary_secrets = 1;
   g_summary_checks = 0;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_summary(tx, op, 1, &summary) == DB2_VAULT_REWRAP_OK);
   g_verify_page_rows = 1;
   g_secret_id = 1;
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   g_secret_id = 2;
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 1, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_INTEGRITY);
   assert(count == 0 && all_zero(&secret, sizeof(secret)));
   db2_vault_rewrap_tx_rollback(&tx);
   g_verify_page_rows = 0;
}

static void test_edge_response_states(void)
{
   uint8_t op[16] = {1}, digest[32] = {0};
   db2_vault_rewrap_tx_t *tx = NULL;

#define EXPECT_BAD_STATE(call, response)                                                           \
   do                                                                                              \
   {                                                                                               \
      g_state_response = (response);                                                               \
      assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);                               \
      assert((call) == DB2_VAULT_REWRAP_INTEGRITY);                                                \
      db2_vault_rewrap_tx_rollback(&tx);                                                           \
   } while (0)

   db2_vault_rewrap_secret_t stage_secret;
   db2_vault_rewrap_check_t stage_check;
   db2_vault_rewrap_cursor_t stage_cursor = {0}, stage_next = {0};
   size_t stage_count = 0;
   g_empty_rows = 1;
   g_state_response = "custody_prepared";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_secret_page(tx, op, 1, 0, 1, &stage_secret, 1, &stage_count) ==
          DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_check_page(tx, op, 1, &stage_cursor, 1, &stage_check, 1,
                                             &stage_count, &stage_next) == DB2_VAULT_REWRAP_OK);
   g_empty_rows = 0;
   assert(db2_vault_rewrap_stage_finish(tx, op, 1, &(db2_vault_rewrap_inventory_summary_t){0}) ==
          DB2_VAULT_REWRAP_INTEGRITY);
   db2_vault_rewrap_tx_rollback(&tx);
   EXPECT_BAD_STATE(db2_vault_rewrap_mark_committing(tx, op, 1), "wraps_staged");
   EXPECT_BAD_STATE(db2_vault_rewrap_mark_resealed(tx, op, 1, digest), "reseal_committing");
   EXPECT_BAD_STATE(db2_vault_rewrap_promote(tx, op, 1), "resealed");
   EXPECT_BAD_STATE(db2_vault_rewrap_abort(tx, op, 1, "failure"), "completed");
   EXPECT_BAD_STATE(db2_vault_rewrap_recovery_required(tx, op, 1, "failure"), "aborted");

   vault_tpm2_reseal_receipt_t receipt = {.old_generation = 7, .new_generation = 8};
   memcpy(receipt.operation_id, op, sizeof(op));
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
   assert(vault_reseal_receipt_encode(&receipt, wire) == 0);
   EXPECT_BAD_STATE(db2_vault_rewrap_record_prepared(tx, op, 1, 7, 8, wire), "preparing");

   g_state_response = "resealed";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_mark_resealed(tx, op, 1, digest) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);
   g_state_response = "aborted";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_abort(tx, op, 1, "failure") == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);
   g_state_response = "recovery_required";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_recovery_required(tx, op, 1, "failure") == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);

   db2_vault_rewrap_verify_summary_t summary;
   db2_vault_rewrap_secret_t secret;
   db2_vault_rewrap_check_t check;
   db2_vault_rewrap_cursor_t cursor = {0}, next = {0};
   size_t count;
   g_summary_secrets = g_summary_checks = 0;
   g_verify_page_rows = 0;
   g_state_response = "promoted";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_summary(tx, op, 1, &summary) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_check_page(tx, op, 1, &cursor, 1, &check, 1, &count, &next) ==
          DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_crypto_ack(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_complete(tx, op, 1, summary.receipt_digest, summary.inventory_digest,
                                    summary.stage_digest) == DB2_VAULT_REWRAP_INTEGRITY);
   db2_vault_rewrap_tx_rollback(&tx);

   cursor = (db2_vault_rewrap_cursor_t){0};
   next = (db2_vault_rewrap_cursor_t){0};
   g_state_response = "completed";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_summary(tx, op, 1, &summary) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_secret_page(tx, op, 1, 0, 1, &secret, 1, &count) ==
          DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_check_page(tx, op, 1, &cursor, 1, &check, 1, &count, &next) ==
          DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_crypto_ack(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_complete(tx, op, 1, summary.receipt_digest, summary.inventory_digest,
                                    summary.stage_digest) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);

   g_state_response = "promoted";
#undef EXPECT_BAD_STATE
}

static void test_output_clear_helpers(void)
{
   db2_vault_rewrap_secret_t secrets[2];
   db2_vault_rewrap_check_t checks[2];
   db2_vault_rewrap_cursor_t cursor;
   db2_vault_rewrap_verify_summary_t summary;
   memset(secrets, 0xa5, sizeof(secrets));
   memset(checks, 0xa5, sizeof(checks));
   memset(&cursor, 0xa5, sizeof(cursor));
   memset(&summary, 0xa5, sizeof(summary));
   db2_vault_rewrap_secret_clear(secrets, 2);
   db2_vault_rewrap_check_clear(checks, 2);
   db2_vault_rewrap_cursor_clear(&cursor);
   db2_vault_rewrap_verify_summary_clear(&summary);
   for (size_t i = 0; i < sizeof(secrets); i++)
      assert(((const unsigned char *)secrets)[i] == 0);
   for (size_t i = 0; i < sizeof(checks); i++)
      assert(((const unsigned char *)checks)[i] == 0);
   for (size_t i = 0; i < sizeof(cursor); i++)
      assert(((const unsigned char *)&cursor)[i] == 0);
   for (size_t i = 0; i < sizeof(summary); i++)
      assert(((const unsigned char *)&summary)[i] == 0);
}

int main(void)
{
   test_reseal_contract();
   test_sqlstate_mapping();
   test_public_handle_phases();
   test_transaction_kind_gates();
   test_prepare_and_followup_failures();
   test_receipt_identity_binding();
   test_page_cap_rejection();
   test_operation_id_binding_and_failure_clear();
   test_page_clearing_and_exhaustion();
   test_ops_vtable_complete();
   test_authoritative_verification_exhaustion();
   test_edge_response_states();
   test_output_clear_helpers();
   puts("org_vault_rewrap: public phase/SQLSTATE tests passed");
   return 0;
}
