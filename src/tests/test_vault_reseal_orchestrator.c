#include "modules/vault/vault_reseal_orchestrator.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct db2_vault_rewrap_tx
{
   int dummy;
};

static struct db2_vault_rewrap_tx g_tx;
static db2_vault_rewrap_state_t g_state;
static vault_tpm2_reseal_status_t g_status;
static int g_supported = 1, g_calls[40];
static int g_forward, g_missing, g_guard_with_result = VAULT_MAINTENANCE_OK;
static db2_vault_rewrap_result_t g_snapshot_result = DB2_VAULT_REWRAP_OK;
static db2_vault_rewrap_result_t g_tx_commit_result = DB2_VAULT_REWRAP_OK;
static db2_vault_rewrap_result_t g_stage_finish_result = DB2_VAULT_REWRAP_OK;
static int g_tx_commit_retain;
static int g_guard_sync_result = VAULT_MAINTENANCE_OK;
static int g_guard_seal_result = VAULT_MAINTENANCE_OK;
static int g_guard_end_result = VAULT_MAINTENANCE_OK;
static int g_prepare_result = VAULT_TPM2_RESEAL_OK, g_prepare_lost_response;
static vault_tpm2_reseal_status_t g_prepare_after_status = VAULT_TPM2_RESEAL_PREPARED;
static int g_nv_result = VAULT_TPM2_RESEAL_OK;
static uint64_t g_nv_generation = 7;
static int g_secret_page_oversize, g_check_page_oversize, g_check_cursor_stale;
static int g_sequence, g_first_order[40], g_last_order[40];
static int64_t g_secret_count, g_check_count;
static vault_tpm2_reseal_receipt_t g_receipt;
static uint8_t g_wire[VAULT_RESEAL_RECEIPT_V1_LEN];

enum
{
   C_SNAPSHOT,
   C_PREPARE,
   C_DISCOVER,
   C_RECOVER,
   C_STATUS,
   C_COMMIT,
   C_ABORT_CUSTODY,
   C_ABORT_DB,
   C_QUARANTINE,
   C_STAGE_FINISH,
   C_MARK_COMMITTING,
   C_MARK_RESEALED,
   C_PROMOTE,
   C_VERIFY_ACK,
   C_COMPLETE,
   C_GUARD_BEGIN,
   C_GUARD_SYNC,
   C_GUARD_WITH,
   C_GUARD_END,
   C_BEGIN,
   C_STAGE_DEK,
   C_STAGE_CHECK,
   C_WRAP,
   C_UNWRAP,
   C_CHECK_WRAP,
   C_CHECK_VERIFY,
   C_TX_COMMIT,
   C_NV,
   C_RECORD,
   C_GUARD_SEAL,
   C_TX_ROLLBACK,
   C_SECRET_PAGE,
   C_CHECK_PAGE,
};

static void called(int call)
{
   g_calls[call]++;
   int order = ++g_sequence;
   if (!g_first_order[call])
      g_first_order[call] = order;
   g_last_order[call] = order;
}

void db2_vault_rewrap_snapshot_clear(db2_vault_rewrap_snapshot_t *s)
{
   if (s)
      OPENSSL_cleanse(s, sizeof(*s));
}
void db2_vault_rewrap_secret_clear(db2_vault_rewrap_secret_t *r, size_t n)
{
   if (r)
      OPENSSL_cleanse(r, n * sizeof(*r));
}
void db2_vault_rewrap_check_clear(db2_vault_rewrap_check_t *r, size_t n)
{
   if (r)
      OPENSSL_cleanse(r, n * sizeof(*r));
}
void db2_vault_rewrap_cursor_clear(db2_vault_rewrap_cursor_t *c)
{
   if (c)
      OPENSSL_cleanse(c, sizeof(*c));
}
void db2_vault_rewrap_verify_summary_clear(db2_vault_rewrap_verify_summary_t *s)
{
   if (s)
      OPENSSL_cleanse(s, sizeof(*s));
}

static db2_vault_rewrap_result_t tx_begin(db2_vault_rewrap_tx_t **t)
{
   *t = &g_tx;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t tx_commit(db2_vault_rewrap_tx_t **t)
{
   called(C_TX_COMMIT);
   db2_vault_rewrap_result_t result = g_tx_commit_result;
   if (!g_tx_commit_retain)
      *t = NULL;
   else
   {
      g_tx_commit_retain = 0;
      g_tx_commit_result = DB2_VAULT_REWRAP_OK;
   }
   return result;
}
static void tx_rollback(db2_vault_rewrap_tx_t **t)
{
   called(C_TX_ROLLBACK);
   *t = NULL;
}
static db2_vault_rewrap_result_t snapshot(const uint8_t op[16], db2_vault_rewrap_snapshot_t *s)
{
   g_calls[C_SNAPSHOT]++;
   if (g_snapshot_result != DB2_VAULT_REWRAP_OK)
      return g_snapshot_result;
   if (g_missing)
      return DB2_VAULT_REWRAP_NOT_FOUND;
   memset(s, 0, sizeof(*s));
   memcpy(s->operation_id, op, 16);
   s->state = g_state;
   s->seal_epoch = 2;
   s->fencing_token = 3;
   s->old_generation = 7;
   s->new_generation = 8;
   if (g_state != DB2_VAULT_REWRAP_PREPARING && g_state != DB2_VAULT_REWRAP_ABORTED)
   {
      s->has_receipt = 1;
      memcpy(s->receipt, g_wire, sizeof(g_wire));
      assert(vault_reseal_receipt_digest(g_wire, s->receipt_digest) == 0);
   }
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t begin_op(db2_vault_rewrap_tx_t *t, const char *a, const char *r,
                                          const uint8_t o[16], int64_t g, int64_t ng, int64_t *e,
                                          int64_t *f, db2_vault_rewrap_state_t *s)
{
   (void)t;
   (void)a;
   (void)r;
   (void)o;
   g_calls[C_BEGIN]++;
   if (g_forward)
   {
      assert(g == 7 && ng == 8);
      if (g_missing)
      {
         g_missing = 0;
         g_state = DB2_VAULT_REWRAP_PREPARING;
      }
      *e = 2;
      *f = 3;
      *s = g_state;
      return DB2_VAULT_REWRAP_OK;
   }
   return DB2_VAULT_REWRAP_TRANSIENT;
}
#define EDGE(name, call)                                                                           \
   static db2_vault_rewrap_result_t name(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f) \
   {                                                                                               \
      (void)t;                                                                                     \
      (void)o;                                                                                     \
      (void)f;                                                                                     \
      g_calls[call]++;                                                                             \
      if (g_forward)                                                                               \
      {                                                                                            \
         g_state = (call) == C_MARK_COMMITTING ? DB2_VAULT_REWRAP_RESEAL_COMMITTING                \
                                               : DB2_VAULT_REWRAP_PROMOTED;                        \
         return DB2_VAULT_REWRAP_OK;                                                               \
      }                                                                                            \
      return DB2_VAULT_REWRAP_TRANSIENT;                                                           \
   }
EDGE(mark_committing, C_MARK_COMMITTING)
EDGE(promote, C_PROMOTE)
static db2_vault_rewrap_result_t record(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                        int64_t g, int64_t ng, const uint8_t w[208])
{
   (void)t;
   (void)o;
   (void)f;
   (void)g;
   (void)ng;
   (void)w;
   called(C_RECORD);
   if (g_forward)
   {
      g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
      return DB2_VAULT_REWRAP_OK;
   }
   return DB2_VAULT_REWRAP_TRANSIENT;
}
static db2_vault_rewrap_result_t secret_page(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                             int64_t f, int64_t a, int l,
                                             db2_vault_rewrap_secret_t *r, size_t cap, size_t *n)
{
   (void)t;
   (void)o;
   (void)f;
   called(C_SECRET_PAGE);
   if (g_secret_page_oversize)
   {
      *n = DB2_VAULT_REWRAP_PAGE_MAX + 1u;
      return DB2_VAULT_REWRAP_OK;
   }
   int64_t total = g_forward ? g_secret_count : 0;
   size_t take = a < total ? (size_t)(total - a) : 0;
   if (take > (size_t)l)
      take = (size_t)l;
   if (take > cap)
      take = cap;
   for (size_t i = 0; i < take; i++)
   {
      r[i].source_id = a + (int64_t)i + 1;
      memset(r[i].wrapped_dek, 1, sizeof(r[i].wrapped_dek));
   }
   *n = take;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t check_page(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                            int64_t f, const db2_vault_rewrap_cursor_t *a, int l,
                                            db2_vault_rewrap_check_t *r, size_t cap, size_t *n,
                                            db2_vault_rewrap_cursor_t *next)
{
   (void)t;
   (void)o;
   (void)f;
   called(C_CHECK_PAGE);
   if (g_check_page_oversize)
   {
      *n = DB2_VAULT_REWRAP_PAGE_MAX + 1u;
      return DB2_VAULT_REWRAP_OK;
   }
   int64_t pos = 0;
   if (a->len)
   {
      char text[32];
      assert(a->len < sizeof(text));
      memcpy(text, a->bytes, a->len);
      text[a->len] = 0;
      pos = strtoll(text, NULL, 10);
   }
   int64_t total = g_forward ? g_check_count : 0;
   size_t take = pos < total ? (size_t)(total - pos) : 0;
   if (take > (size_t)l)
      take = (size_t)l;
   if (take > cap)
      take = cap;
   for (size_t i = 0; i < take; i++)
   {
      snprintf(r[i].principal, sizeof(r[i].principal), "principal-%lld",
               (long long)(pos + (int64_t)i));
      r[i].kek_check_len = VAULT_WRAPPED_DEK_LEN;
      memset(r[i].kek_check, 3, sizeof(r[i].kek_check));
   }
   pos += (int64_t)take;
   *next = *a;
   if (take)
   {
      memset(next, 0, sizeof(*next));
      int written = snprintf((char *)next->bytes, sizeof(next->bytes), "%020lld", (long long)pos);
      assert(written == 20);
      next->len = (size_t)written;
      if (g_check_cursor_stale)
         *next = *a;
   }
   *n = take;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t stage_dek(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                           const db2_vault_rewrap_secret_t *s, const uint8_t w[40])
{
   (void)t;
   (void)o;
   (void)f;
   (void)s;
   (void)w;
   g_calls[C_STAGE_DEK]++;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t stage_check(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                             int64_t f, const db2_vault_rewrap_check_t *s,
                                             const uint8_t *w, size_t n)
{
   (void)t;
   (void)o;
   (void)f;
   (void)s;
   (void)w;
   (void)n;
   g_calls[C_STAGE_CHECK]++;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t stage_finish(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                              int64_t f)
{
   (void)t;
   (void)o;
   (void)f;
   g_calls[C_STAGE_FINISH]++;
   if (g_stage_finish_result != DB2_VAULT_REWRAP_OK)
      return g_stage_finish_result;
   if (g_forward)
   {
      g_state = DB2_VAULT_REWRAP_WRAPS_STAGED;
      return DB2_VAULT_REWRAP_OK;
   }
   return DB2_VAULT_REWRAP_TRANSIENT;
}
static db2_vault_rewrap_result_t mark_resealed(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                               int64_t f, const uint8_t d[32])
{
   (void)t;
   (void)o;
   (void)f;
   (void)d;
   g_calls[C_MARK_RESEALED]++;
   if (g_forward)
   {
      g_state = DB2_VAULT_REWRAP_RESEALED;
      return DB2_VAULT_REWRAP_OK;
   }
   return DB2_VAULT_REWRAP_TRANSIENT;
}
static db2_vault_rewrap_result_t abort_db(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                          const char *x)
{
   (void)t;
   (void)o;
   (void)f;
   (void)x;
   called(C_ABORT_DB);
   g_state = DB2_VAULT_REWRAP_ABORTED;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t quarantine(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                            int64_t f, const char *x)
{
   (void)t;
   (void)o;
   (void)f;
   (void)x;
   g_calls[C_QUARANTINE]++;
   g_state = DB2_VAULT_REWRAP_RECOVERY_REQUIRED;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t summary(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                         db2_vault_rewrap_verify_summary_t *s)
{
   (void)t;
   (void)o;
   (void)f;
   memset(s, 0, sizeof(*s));
   s->secret_count = g_secret_count;
   s->check_count = g_check_count;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t verify_ack(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                            int64_t f)
{
   (void)t;
   (void)o;
   (void)f;
   g_calls[C_VERIFY_ACK]++;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t complete(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                          const uint8_t a[32], const uint8_t b[32],
                                          const uint8_t c[32])
{
   (void)t;
   (void)o;
   (void)f;
   (void)a;
   (void)b;
   (void)c;
   g_calls[C_COMPLETE]++;
   if (g_forward)
   {
      g_state = DB2_VAULT_REWRAP_COMPLETED;
      return DB2_VAULT_REWRAP_OK;
   }
   return DB2_VAULT_REWRAP_TRANSIENT;
}

static const db2_vault_rewrap_ops_t dbops = {.tx_begin = tx_begin,
                                             .tx_commit = tx_commit,
                                             .tx_rollback = tx_rollback,
                                             .snapshot = snapshot,
                                             .begin = begin_op,
                                             .record_prepared = record,
                                             .source_secret_page = secret_page,
                                             .source_check_page = check_page,
                                             .stage_dek = stage_dek,
                                             .stage_check = stage_check,
                                             .stage_finish = stage_finish,
                                             .mark_committing = mark_committing,
                                             .mark_resealed = mark_resealed,
                                             .promote = promote,
                                             .abort = abort_db,
                                             .recovery_required = quarantine,
                                             .verify_summary = summary,
                                             .verify_secret_page = secret_page,
                                             .verify_check_page = check_page,
                                             .verify_crypto_ack = verify_ack,
                                             .complete = complete};

static int supported(void)
{
   return g_supported;
}
static int nv(const char *s, uint64_t *g)
{
   (void)s;
   called(C_NV);
   *g = g_nv_generation;
   return g_nv_result;
}
static int prepare(const uint8_t o[16], uint64_t g, const uint8_t k[32], const char *s,
                   vault_tpm2_reseal_receipt_t *r)
{
   (void)o;
   (void)g;
   (void)k;
   (void)s;
   called(C_PREPARE);
   *r = g_receipt;
   if (g_forward)
      g_status = VAULT_TPM2_RESEAL_PREPARED;
   else if (g_prepare_lost_response)
      g_status = g_prepare_after_status;
   return g_prepare_result;
}
static int discover(const uint8_t o[16], uint64_t g, const char *s, vault_tpm2_reseal_receipt_t *r,
                    vault_tpm2_reseal_status_t *st)
{
   (void)o;
   (void)g;
   (void)s;
   called(C_DISCOVER);
   *st = g_status;
   if (*st != VAULT_TPM2_RESEAL_ABSENT)
      *r = g_receipt;
   return (*st == VAULT_TPM2_RESEAL_CONFLICT || *st == VAULT_TPM2_RESEAL_CORRUPT)
              ? VAULT_TPM2_RESEAL_INTEGRITY
              : VAULT_TPM2_RESEAL_OK;
}
static int recover(const vault_tpm2_reseal_receipt_t *r, const char *s, uint8_t k[32])
{
   (void)r;
   (void)s;
   memset(k, 9, 32);
   g_calls[C_RECOVER]++;
   return VAULT_TPM2_RESEAL_OK;
}
static int status_fn(const vault_tpm2_reseal_receipt_t *r, const char *s,
                     vault_tpm2_reseal_status_t *st)
{
   (void)r;
   (void)s;
   called(C_STATUS);
   *st = g_status;
   return (*st == VAULT_TPM2_RESEAL_CONFLICT || *st == VAULT_TPM2_RESEAL_CORRUPT)
              ? VAULT_TPM2_RESEAL_INTEGRITY
              : VAULT_TPM2_RESEAL_OK;
}
static int custody_commit(const vault_tpm2_reseal_receipt_t *r, const char *s,
                          vault_tpm2_reseal_status_t *st)
{
   (void)r;
   (void)s;
   g_calls[C_COMMIT]++;
   *st = VAULT_TPM2_RESEAL_INSTALLED;
   if (g_forward)
      g_status = *st;
   return VAULT_TPM2_RESEAL_OK;
}
static int custody_abort(const vault_tpm2_reseal_receipt_t *r, const char *s)
{
   (void)r;
   (void)s;
   called(C_ABORT_CUSTODY);
   return VAULT_TPM2_RESEAL_OK;
}
static int guard_begin(void **g)
{
   g_calls[C_GUARD_BEGIN]++;
   *g = &g_tx;
   return VAULT_MAINTENANCE_OK;
}
static int guard_sync(void *g, uint64_t e)
{
   (void)g;
   (void)e;
   g_calls[C_GUARD_SYNC]++;
   return g_guard_sync_result;
}
static int guard_unseal(void *g, const void *p, size_t n)
{
   (void)g;
   (void)p;
   (void)n;
   return VAULT_MAINTENANCE_OK;
}
static int guard_seal(void *g)
{
   (void)g;
   called(C_GUARD_SEAL);
   return g_guard_seal_result;
}
static int guard_with(void *g, vault_maintenance_kek_fn f, void *c)
{
   uint8_t k[32] = {0};
   (void)g;
   g_calls[C_GUARD_WITH]++;
   int rc = f(k, c);
   OPENSSL_cleanse(k, sizeof(k));
   return g_guard_with_result == VAULT_MAINTENANCE_OK ? rc : g_guard_with_result;
}
static int guard_end(void **g)
{
   g_calls[C_GUARD_END]++;
   *g = NULL;
   return g_guard_end_result;
}
static int random_bytes(uint8_t *p, size_t n)
{
   memset(p, 7, n);
   return 0;
}
static int wrap(const uint8_t k[32], const uint8_t d[32], uint8_t w[40])
{
   (void)k;
   (void)d;
   g_calls[C_WRAP]++;
   memset(w, 1, 40);
   return 0;
}
static int unwrap(const uint8_t k[32], const uint8_t w[40], uint8_t d[32])
{
   (void)k;
   (void)w;
   g_calls[C_UNWRAP]++;
   memset(d, 2, 32);
   return 0;
}
static int check_wrap(const uint8_t k[32], uint8_t w[40])
{
   (void)k;
   g_calls[C_CHECK_WRAP]++;
   memset(w, 3, 40);
   return 0;
}
static int check_verify(const uint8_t k[32], const uint8_t w[40])
{
   (void)k;
   (void)w;
   g_calls[C_CHECK_VERIFY]++;
   return 0;
}
static const vault_reseal_custody_ops_t cops = {
    supported,     nv,          prepare,    discover,     recover,     status_fn,  custody_commit,
    custody_abort, guard_begin, guard_sync, guard_unseal, guard_seal,  guard_with, guard_end,
    random_bytes,  wrap,        unwrap,     check_wrap,   check_verify};
static const vault_reseal_orchestrator_deps_t deps = {&dbops, &cops};

static void reset_fakes(void)
{
   memset(g_calls, 0, sizeof(g_calls));
   memset(g_first_order, 0, sizeof(g_first_order));
   memset(g_last_order, 0, sizeof(g_last_order));
   g_sequence = 0;
   g_supported = 1;
   g_forward = 0;
   g_missing = 0;
   g_guard_with_result = VAULT_MAINTENANCE_OK;
   g_guard_sync_result = VAULT_MAINTENANCE_OK;
   g_guard_seal_result = VAULT_MAINTENANCE_OK;
   g_guard_end_result = VAULT_MAINTENANCE_OK;
   g_snapshot_result = DB2_VAULT_REWRAP_OK;
   g_tx_commit_result = DB2_VAULT_REWRAP_OK;
   g_stage_finish_result = DB2_VAULT_REWRAP_OK;
   g_tx_commit_retain = 0;
   g_prepare_result = VAULT_TPM2_RESEAL_OK;
   g_prepare_lost_response = 0;
   g_prepare_after_status = VAULT_TPM2_RESEAL_PREPARED;
   g_nv_result = VAULT_TPM2_RESEAL_OK;
   g_nv_generation = 7;
   g_secret_page_oversize = g_check_page_oversize = g_check_cursor_stale = 0;
   g_secret_count = g_check_count = 0;
}

static vault_reseal_orchestrator_result_t run(db2_vault_rewrap_state_t state,
                                              vault_tpm2_reseal_status_t status)
{
   vault_reseal_orchestrator_request_t r;
   vault_reseal_orchestrator_output_t out;
   uint8_t secret[] = {1, 2, 3};
   memset(&r, 0, sizeof(r));
   r.mode = VAULT_RESEAL_ORCHESTRATOR_RESUME;
   memset(r.operation_id, 4, sizeof(r.operation_id));
   r.actor = "owner";
   r.request_id = "request";
   r.provider_secret = secret;
   r.provider_secret_len = sizeof(secret);
   g_state = state;
   g_status = status;
   reset_fakes();
   g_state = state;
   g_status = status;
   vault_reseal_orchestrator_result_t rc = vault_reseal_orchestrator_run(&r, &deps, &out);
   assert(g_calls[C_GUARD_BEGIN] == 1 && g_calls[C_GUARD_END] == 1);
   assert(out.has_state);
   if (rc == VAULT_RESEAL_ORCHESTRATOR_ABORTED)
      assert(out.state == DB2_VAULT_REWRAP_ABORTED);
   else if (rc == VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED)
      assert(out.state == DB2_VAULT_REWRAP_RECOVERY_REQUIRED);
   else
      assert(out.state == state);
   return rc;
}

static vault_reseal_orchestrator_request_t request(vault_reseal_orchestrator_mode_t mode,
                                                   uint8_t secret[3])
{
   vault_reseal_orchestrator_request_t r;
   memset(&r, 0, sizeof(r));
   r.mode = mode;
   memset(r.operation_id, 4, sizeof(r.operation_id));
   r.actor = "owner";
   r.request_id = "request";
   r.provider_secret = secret;
   r.provider_secret_len = 3;
   return r;
}

static void full_forward_path(void)
{
   uint8_t secret[3] = {1, 2, 3};
   vault_reseal_orchestrator_request_t r = request(VAULT_RESEAL_ORCHESTRATOR_START, secret);
   vault_reseal_orchestrator_output_t out;
   reset_fakes();
   g_forward = 1;
   g_missing = 1;
   g_state = DB2_VAULT_REWRAP_PREPARING;
   g_status = VAULT_TPM2_RESEAL_ABSENT;
   g_secret_count = 2 * DB2_VAULT_REWRAP_PAGE_MAX + 1;
   g_check_count = 2 * DB2_VAULT_REWRAP_PAGE_MAX + 1;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_COMPLETED);
   assert(out.has_state && out.state == DB2_VAULT_REWRAP_COMPLETED);
   assert(g_calls[C_BEGIN] == 1 && g_calls[C_PREPARE] == 1);
   assert(g_calls[C_STAGE_DEK] == g_secret_count && g_calls[C_STAGE_CHECK] == g_check_count);
   assert(g_calls[C_WRAP] == g_secret_count);
   assert(g_calls[C_UNWRAP] == 2 * g_secret_count);
   assert(g_calls[C_CHECK_WRAP] == g_check_count);
   assert(g_calls[C_CHECK_VERIFY] == 2 * g_check_count);
   assert(g_calls[C_GUARD_WITH] == 2 && g_calls[C_COMPLETE] == 1);
   assert(g_calls[C_SECRET_PAGE] == 8 && g_calls[C_CHECK_PAGE] == 8);
}

static void replay_callback_and_terminal_guards(void)
{
   uint8_t secret[3] = {1, 2, 3};
   vault_reseal_orchestrator_output_t out;
   vault_reseal_orchestrator_request_t r = request(VAULT_RESEAL_ORCHESTRATOR_START, secret);
   reset_fakes();
   g_forward = 1;
   g_missing = 0;
   g_state = DB2_VAULT_REWRAP_COMPLETED;
   g_status = VAULT_TPM2_RESEAL_INSTALLED;
   g_secret_count = g_check_count = 0;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_COMPLETED);
   assert(g_calls[C_BEGIN] == 1);
   assert(g_calls[C_GUARD_SYNC] == 0);

   r.mode = VAULT_RESEAL_ORCHESTRATOR_RESUME;
   memset(g_calls, 0, sizeof(g_calls));
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_guard_with_result = VAULT_MAINTENANCE_ERROR;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY);
   assert(g_calls[C_STAGE_FINISH] == 1 && g_calls[C_GUARD_WITH] == 1);
   g_guard_with_result = VAULT_MAINTENANCE_OK;

   memset(g_calls, 0, sizeof(g_calls));
   g_forward = 0;
   g_state = DB2_VAULT_REWRAP_ABORTED;
   g_status = VAULT_TPM2_RESEAL_ABSENT;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_ABORTED);
   assert(g_calls[C_GUARD_SYNC] == 0 && g_calls[C_DISCOVER] == 1 && g_calls[C_STATUS] == 0);

   memset(g_calls, 0, sizeof(g_calls));
   g_state = DB2_VAULT_REWRAP_PREPARING;
   g_snapshot_result = DB2_VAULT_REWRAP_TRANSIENT;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY);
   assert(g_calls[C_SNAPSHOT] == 1 && g_calls[C_GUARD_SYNC] == 0);
   g_snapshot_result = DB2_VAULT_REWRAP_OK;

   memset(g_calls, 0, sizeof(g_calls));
   g_guard_sync_result = VAULT_MAINTENANCE_EPOCH;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY);
   assert(g_calls[C_GUARD_SYNC] == 1 && g_calls[C_DISCOVER] == 0);
   g_guard_sync_result = VAULT_MAINTENANCE_BUSY;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_BUSY);
   g_guard_sync_result = VAULT_MAINTENANCE_ERROR;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY);
   g_guard_sync_result = VAULT_MAINTENANCE_OK;
}

static void pagination_defenses(void)
{
   uint8_t secret[3] = {1, 2, 3};
   vault_reseal_orchestrator_request_t r = request(VAULT_RESEAL_ORCHESTRATOR_RESUME, secret);
   vault_reseal_orchestrator_output_t out;

   reset_fakes();
   g_forward = 1;
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_secret_count = g_check_count = 1;
   g_secret_page_oversize = 1;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) ==
          VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
   assert(g_calls[C_STAGE_DEK] == 0 && g_calls[C_QUARANTINE] == 1);

   reset_fakes();
   g_forward = 1;
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_secret_count = g_check_count = 1;
   g_check_page_oversize = 1;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) ==
          VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
   assert(g_calls[C_STAGE_DEK] == 1 && g_calls[C_STAGE_CHECK] == 0 && g_calls[C_QUARANTINE] == 1);

   reset_fakes();
   g_forward = 1;
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_secret_count = g_check_count = 1;
   g_check_cursor_stale = 1;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) ==
          VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
   assert(g_calls[C_STAGE_CHECK] == 0 && g_calls[C_QUARANTINE] == 1);
}

static void callback_result_typing(void)
{
   uint8_t secret[3] = {1, 2, 3};
   vault_reseal_orchestrator_request_t r = request(VAULT_RESEAL_ORCHESTRATOR_RESUME, secret);
   vault_reseal_orchestrator_output_t out;

   reset_fakes();
   g_forward = 1;
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_stage_finish_result = DB2_VAULT_REWRAP_BUSY;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_BUSY);

   reset_fakes();
   g_forward = 1;
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_stage_finish_result = DB2_VAULT_REWRAP_INVALID;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
}

static void assert_critical_cell_calls(db2_vault_rewrap_state_t state,
                                       vault_tpm2_reseal_status_t status)
{
   if (state == DB2_VAULT_REWRAP_ABORTED)
   {
      assert(g_calls[C_GUARD_SYNC] == 0 && g_calls[C_DISCOVER] == 1 && g_calls[C_STATUS] == 0);
      assert(g_calls[C_PREPARE] == 0 && g_calls[C_COMMIT] == 0 && g_calls[C_ABORT_CUSTODY] == 0);
      assert(g_calls[C_GUARD_WITH] == 0 && g_calls[C_STAGE_FINISH] == 0 &&
             g_calls[C_COMPLETE] == 0);
   }
   if (state == DB2_VAULT_REWRAP_RECOVERY_REQUIRED)
   {
      assert(g_calls[C_GUARD_SYNC] == 0 && g_calls[C_DISCOVER] == 0 && g_calls[C_STATUS] == 0);
      assert(g_calls[C_PREPARE] == 0 && g_calls[C_COMMIT] == 0 && g_calls[C_ABORT_CUSTODY] == 0);
      assert(g_calls[C_GUARD_WITH] == 0 && g_calls[C_STAGE_FINISH] == 0 &&
             g_calls[C_COMPLETE] == 0);
   }
   if (state == DB2_VAULT_REWRAP_COMPLETED)
   {
      assert(g_calls[C_STATUS] == 1 && g_calls[C_GUARD_SYNC] == 0);
      assert(g_calls[C_DISCOVER] == 0 && g_calls[C_PREPARE] == 0 && g_calls[C_COMMIT] == 0);
      assert(g_calls[C_GUARD_WITH] == 0 && g_calls[C_MARK_COMMITTING] == 0 &&
             g_calls[C_PROMOTE] == 0);
   }
   if (state == DB2_VAULT_REWRAP_PREPARING)
   {
      assert(g_calls[C_STATUS] == 0 && g_calls[C_COMMIT] == 0 && g_calls[C_ABORT_CUSTODY] == 0);
      assert(g_calls[C_GUARD_WITH] == 0 && g_calls[C_MARK_COMMITTING] == 0 &&
             g_calls[C_PROMOTE] == 0);
      if (status == VAULT_TPM2_RESEAL_ABSENT)
         assert(g_calls[C_DISCOVER] == 1 && g_calls[C_PREPARE] == 1 && g_calls[C_RECORD] == 1);
   }
   if (state == DB2_VAULT_REWRAP_WRAPS_STAGED && status == VAULT_TPM2_RESEAL_PREPARED)
   {
      assert(g_calls[C_STATUS] == 1 && g_calls[C_MARK_COMMITTING] == 1);
      assert(g_calls[C_DISCOVER] == 0 && g_calls[C_PREPARE] == 0 && g_calls[C_COMMIT] == 0);
      assert(g_calls[C_GUARD_WITH] == 0 && g_calls[C_MARK_RESEALED] == 0 &&
             g_calls[C_PROMOTE] == 0);
   }
   if (state == DB2_VAULT_REWRAP_CUSTODY_PREPARED && status == VAULT_TPM2_RESEAL_ABSENT)
   {
      assert(g_calls[C_STATUS] == 1 && g_calls[C_ABORT_CUSTODY] == 1 && g_calls[C_NV] == 1);
      assert(g_calls[C_ABORT_DB] == 1 && g_calls[C_COMMIT] == 0 && g_calls[C_GUARD_WITH] == 0);
   }
   if (state == DB2_VAULT_REWRAP_RESEAL_COMMITTING && status == VAULT_TPM2_RESEAL_INSTALLED)
   {
      assert(g_calls[C_STATUS] == 1 && g_calls[C_COMMIT] == 1 && g_calls[C_MARK_RESEALED] == 1);
      assert(g_calls[C_DISCOVER] == 0 && g_calls[C_PREPARE] == 0 && g_calls[C_GUARD_WITH] == 0);
      assert(g_calls[C_MARK_COMMITTING] == 0 && g_calls[C_PROMOTE] == 0);
   }
}

static void exhaustive_matrix(void)
{
   for (int state = DB2_VAULT_REWRAP_PREPARING; state <= DB2_VAULT_REWRAP_RECOVERY_REQUIRED;
        state++)
      for (int status = VAULT_TPM2_RESEAL_ABSENT; status <= VAULT_TPM2_RESEAL_CORRUPT; status++)
      {
         vault_reseal_orchestrator_result_t rc =
             run((db2_vault_rewrap_state_t)state, (vault_tpm2_reseal_status_t)status);
         if (state == DB2_VAULT_REWRAP_ABORTED)
            assert(rc == (status == VAULT_TPM2_RESEAL_ABSENT
                              ? VAULT_RESEAL_ORCHESTRATOR_ABORTED
                              : VAULT_RESEAL_ORCHESTRATOR_INTEGRITY));
         else if (state == DB2_VAULT_REWRAP_RECOVERY_REQUIRED)
            assert(rc == VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
         else if (state == DB2_VAULT_REWRAP_COMPLETED)
            assert(rc ==
                   ((status == VAULT_TPM2_RESEAL_INSTALLED || status == VAULT_TPM2_RESEAL_CLEANED)
                        ? VAULT_RESEAL_ORCHESTRATOR_COMPLETED
                        : VAULT_RESEAL_ORCHESTRATOR_INTEGRITY));
         else if ((status == VAULT_TPM2_RESEAL_CONFLICT || status == VAULT_TPM2_RESEAL_CORRUPT))
         {
            assert(rc == VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
         }
         else
            assert(rc == VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY ||
                   rc == VAULT_RESEAL_ORCHESTRATOR_ABORTED ||
                   rc == VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
         assert_critical_cell_calls((db2_vault_rewrap_state_t)state,
                                    (vault_tpm2_reseal_status_t)status);
      }
}

static void uncertainty_and_abort_ordering(void)
{
   uint8_t secret[3] = {1, 2, 3};
   vault_reseal_orchestrator_request_t r = request(VAULT_RESEAL_ORCHESTRATOR_RESUME, secret);
   vault_reseal_orchestrator_output_t out;

   reset_fakes();
   g_forward = 1;
   g_state = DB2_VAULT_REWRAP_WRAPS_STAGED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_tx_commit_result = DB2_VAULT_REWRAP_TRANSIENT;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY);
   assert(g_calls[C_MARK_COMMITTING] == 1 && g_calls[C_TX_COMMIT] == 1);
   assert(g_calls[C_COMMIT] == 0 && g_calls[C_MARK_RESEALED] == 0 && g_calls[C_GUARD_WITH] == 0);

   reset_fakes();
   g_forward = 1;
   g_state = DB2_VAULT_REWRAP_WRAPS_STAGED;
   g_status = VAULT_TPM2_RESEAL_PREPARED;
   g_tx_commit_result = DB2_VAULT_REWRAP_INVALID;
   g_tx_commit_retain = 1;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) ==
          VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
   assert(g_calls[C_TX_COMMIT] >= 1 && g_calls[C_TX_ROLLBACK] == 1);

   reset_fakes();
   g_state = DB2_VAULT_REWRAP_PREPARING;
   g_status = VAULT_TPM2_RESEAL_ABSENT;
   g_prepare_result = VAULT_TPM2_RESEAL_ERR;
   g_prepare_lost_response = 1;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY);
   assert(g_calls[C_PREPARE] == 1 && g_calls[C_DISCOVER] == 2 && g_calls[C_RECORD] == 1);
   assert(g_calls[C_RECOVER] == 1 && g_calls[C_ABORT_CUSTODY] == 0 && g_calls[C_QUARANTINE] == 0);

   const vault_tpm2_reseal_status_t ahead[] = {
       VAULT_TPM2_RESEAL_NV_ADVANCED, VAULT_TPM2_RESEAL_INSTALLED, VAULT_TPM2_RESEAL_CLEANED};
   for (size_t i = 0; i < sizeof(ahead) / sizeof(ahead[0]); i++)
   {
      reset_fakes();
      g_state = DB2_VAULT_REWRAP_PREPARING;
      g_status = VAULT_TPM2_RESEAL_ABSENT;
      g_prepare_result = VAULT_TPM2_RESEAL_ERR;
      g_prepare_lost_response = 1;
      g_prepare_after_status = ahead[i];
      assert(vault_reseal_orchestrator_run(&r, &deps, &out) ==
             VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
      assert(out.state == DB2_VAULT_REWRAP_RECOVERY_REQUIRED);
      assert(g_calls[C_PREPARE] == 1 && g_calls[C_DISCOVER] == 2);
      assert(g_calls[C_QUARANTINE] == 1 && g_calls[C_RECORD] == 0 && g_calls[C_RECOVER] == 0);
   }

   reset_fakes();
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_ABSENT;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_ABORTED);
   assert(g_calls[C_ABORT_CUSTODY] == 1 && g_calls[C_DISCOVER] == 2 && g_calls[C_NV] == 1 &&
          g_calls[C_ABORT_DB] == 1);
   assert(g_first_order[C_ABORT_CUSTODY] < g_first_order[C_DISCOVER]);
   assert(g_first_order[C_DISCOVER] < g_first_order[C_NV]);
   assert(g_first_order[C_NV] < g_first_order[C_ABORT_DB]);
   assert(g_first_order[C_ABORT_DB] < g_last_order[C_DISCOVER]);
   assert(g_calls[C_GUARD_WITH] == 0 && g_calls[C_COMMIT] == 0);

   reset_fakes();
   g_state = DB2_VAULT_REWRAP_CUSTODY_PREPARED;
   g_status = VAULT_TPM2_RESEAL_ABSENT;
   g_nv_generation = 8;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) ==
          VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED);
   assert(g_calls[C_ABORT_CUSTODY] == 1 && g_calls[C_NV] == 1);
   assert(g_calls[C_ABORT_DB] == 0 && g_calls[C_QUARANTINE] == 1);
}

static void teardown_failures(void)
{
   uint8_t secret[3] = {1, 2, 3};
   vault_reseal_orchestrator_request_t r = request(VAULT_RESEAL_ORCHESTRATOR_RESUME, secret);
   vault_reseal_orchestrator_output_t out;

   reset_fakes();
   g_state = DB2_VAULT_REWRAP_COMPLETED;
   g_status = VAULT_TPM2_RESEAL_INSTALLED;
   g_guard_seal_result = VAULT_MAINTENANCE_ERROR;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_ERROR);
   assert(g_calls[C_GUARD_SEAL] == 1 && g_calls[C_GUARD_END] == 1);

   reset_fakes();
   g_state = DB2_VAULT_REWRAP_COMPLETED;
   g_status = VAULT_TPM2_RESEAL_INSTALLED;
   g_guard_end_result = VAULT_MAINTENANCE_ERROR;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_ERROR);
   assert(g_calls[C_GUARD_SEAL] == 1 && g_calls[C_GUARD_END] == 1);
}

static void validation_before_effects(void)
{
   uint8_t secret[3] = {1, 2, 3};
   vault_reseal_orchestrator_request_t r = request(VAULT_RESEAL_ORCHESTRATOR_RESUME, secret);
   vault_reseal_orchestrator_output_t out;
   char long_actor[577], long_request[202];
   memset(long_actor, 'a', sizeof(long_actor));
   long_actor[sizeof(long_actor) - 1] = 0;
   memset(long_request, 'r', sizeof(long_request));
   long_request[sizeof(long_request) - 1] = 0;

   reset_fakes();
   assert(vault_reseal_orchestrator_run(&r, &deps, NULL) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   assert(g_calls[C_GUARD_BEGIN] == 0 && g_calls[C_SNAPSHOT] == 0);

   vault_reseal_orchestrator_request_t bad = r;
   bad.mode = 0;
   memset(&out, 0xa5, sizeof(out));
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   assert(!out.has_state);

   bad = r;
   bad.actor = NULL;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   bad = r;
   bad.actor = long_actor;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   bad = r;
   bad.request_id = NULL;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   bad = r;
   bad.request_id = long_request;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   bad = r;
   bad.provider_secret = NULL;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   bad = r;
   bad.provider_secret_len = 0;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   bad = r;
   bad.provider_secret_len = VAULT_RESEAL_ORCHESTRATOR_SECRET_MAX + 1;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   bad = r;
   secret[1] = 0;
   assert(vault_reseal_orchestrator_run(&bad, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_INVALID);
   secret[1] = 2;
   assert(g_calls[C_GUARD_BEGIN] == 0 && g_calls[C_SNAPSHOT] == 0 && g_calls[C_NV] == 0);

   reset_fakes();
   g_supported = 0;
   assert(vault_reseal_orchestrator_run(&r, &deps, &out) == VAULT_RESEAL_ORCHESTRATOR_UNSUPPORTED);
   assert(g_calls[C_GUARD_BEGIN] == 0 && g_calls[C_SNAPSHOT] == 0);
}

int main(void)
{
   memset(&g_receipt, 0, sizeof(g_receipt));
   memset(g_receipt.operation_id, 4, sizeof(g_receipt.operation_id));
   g_receipt.old_generation = 7;
   g_receipt.new_generation = 8;
   assert(vault_reseal_receipt_encode(&g_receipt, g_wire) == 0);
   validation_before_effects();
   exhaustive_matrix();
   full_forward_path();
   replay_callback_and_terminal_guards();
   pagination_defenses();
   callback_result_typing();
   uncertainty_and_abort_ordering();
   teardown_failures();
   puts("vault_reseal_orchestrator: exhaustive DB x custody tests passed");
   return 0;
}
