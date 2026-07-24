#include "vault_reseal_orchestrator.h"

#include "vault_crypto.h"
#include "vault_kek_check.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct
{
   uint8_t new_kek[VAULT_KEK_LEN];
   uint8_t dek[VAULT_DEK_LEN];
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   int has_new_kek;
} secret_workspace_t;

typedef struct
{
   void *p;
   size_t n;
} protected_arena_t;

typedef struct
{
   const vault_reseal_orchestrator_deps_t *deps;
   const db2_vault_rewrap_snapshot_t *snapshot;
   secret_workspace_t *workspace;
   vault_mutation_budget_t *budget;
   uint64_t *progress;
   vault_reseal_orchestrator_result_t result;
} crypto_context_t;

static int budget_advance(vault_mutation_budget_t *budget, uint64_t *progress)
{
   return budget && progress && *progress < UINT64_MAX &&
                  vault_mutation_budget_progress(budget, 1, ++*progress) == 0
              ? 0
              : -1;
}

static int arena_new(size_t need, protected_arena_t *a)
{
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   long ps = sysconf(_SC_PAGESIZE);
   if (!a || ps <= 0 || need > SIZE_MAX - (size_t)ps)
      return -1;
   a->n = (need + (size_t)ps - 1) / (size_t)ps * (size_t)ps;
   a->p = mmap(NULL, a->n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (a->p == MAP_FAILED)
   {
      a->p = NULL;
      a->n = 0;
      return -1;
   }
   if (mlock(a->p, a->n) != 0 || madvise(a->p, a->n, MADV_DONTDUMP) != 0 ||
       madvise(a->p, a->n, MADV_WIPEONFORK) != 0)
   {
      OPENSSL_cleanse(a->p, a->n);
      (void)munlock(a->p, a->n);
      (void)munmap(a->p, a->n);
      a->p = NULL;
      a->n = 0;
      return -1;
   }
   return 0;
#else
   (void)need;
   if (a)
      memset(a, 0, sizeof(*a));
   return -1;
#endif
}

static void arena_free(protected_arena_t *a)
{
   if (!a || !a->p)
      return;
   OPENSSL_cleanse(a->p, a->n);
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   (void)munlock(a->p, a->n);
   (void)munmap(a->p, a->n);
#endif
   a->p = NULL;
   a->n = 0;
}

static int request_valid(const vault_reseal_orchestrator_request_t *r)
{
   if (!r ||
       (r->mode != VAULT_RESEAL_ORCHESTRATOR_START &&
        r->mode != VAULT_RESEAL_ORCHESTRATOR_RESUME) ||
       !r->actor || !r->request_id || !r->provider_secret || !r->provider_secret_len ||
       !r->budget || r->provider_secret_len > VAULT_RESEAL_ORCHESTRATOR_SECRET_MAX)
      return 0;
   size_t an = strnlen(r->actor, 576), rn = strnlen(r->request_id, 201);
   return an >= 1 && an <= 575 && rn >= 1 && rn <= 200 &&
          memchr(r->provider_secret, 0, r->provider_secret_len) == NULL;
}

static int deps_valid(const vault_reseal_orchestrator_deps_t *d)
{
   const db2_vault_rewrap_ops_t *b = d ? d->db : NULL;
   const vault_reseal_custody_ops_t *c = d ? d->custody : NULL;
   return b && c && b->tx_begin && b->tx_commit && b->tx_rollback && b->snapshot && b->begin &&
          b->record_prepared && b->source_secret_page && b->source_check_page && b->stage_dek &&
          b->stage_check && b->inventory_summary && b->stage_finish && b->mark_committing &&
          b->mark_resealed && b->promote && b->abort && b->recovery_required && b->verify_summary &&
          b->verify_secret_page && b->verify_check_page && b->verify_crypto_ack && b->complete &&
          c->supported && c->nv_generation && c->prepare && c->discover && c->recover_kek &&
          c->status && c->commit && c->abort && c->guard_begin && c->guard_sync_epoch &&
          c->guard_unseal && c->guard_seal && c->guard_with_active_kek && c->guard_end &&
          c->random && c->dek_wrap && c->dek_unwrap && c->check_wrap && c->check_verify;
}

static vault_reseal_orchestrator_result_t db_result(db2_vault_rewrap_result_t r)
{
   switch (r)
   {
   case DB2_VAULT_REWRAP_OK:
      return VAULT_RESEAL_ORCHESTRATOR_COMPLETED;
   case DB2_VAULT_REWRAP_BUSY:
      return VAULT_RESEAL_ORCHESTRATOR_BUSY;
   case DB2_VAULT_REWRAP_CONFLICT:
   case DB2_VAULT_REWRAP_TRANSIENT:
      return VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
   case DB2_VAULT_REWRAP_INTEGRITY:
      return VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
   case DB2_VAULT_REWRAP_INVALID:
      return VAULT_RESEAL_ORCHESTRATOR_INVALID;
   default:
      return VAULT_RESEAL_ORCHESTRATOR_ERROR;
   }
}

static vault_reseal_orchestrator_result_t custody_result(int r)
{
   switch (r)
   {
   case VAULT_TPM2_RESEAL_NOT_BUILT:
      return VAULT_RESEAL_ORCHESTRATOR_UNSUPPORTED;
   case VAULT_TPM2_RESEAL_BUSY:
      return VAULT_RESEAL_ORCHESTRATOR_BUSY;
   case VAULT_TPM2_RESEAL_INTEGRITY:
      return VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
   default:
      return VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
   }
}

static vault_reseal_orchestrator_result_t
guard_callback_result(int guard_result, vault_reseal_orchestrator_result_t callback_result)
{
   if (guard_result == VAULT_MAINTENANCE_OK)
      return callback_result;
   /* The maintenance callback reports every non-success as a generic guard
    * error. Preserve its typed DB/crypto outcome when it actually ran. */
   if (callback_result != VAULT_RESEAL_ORCHESTRATOR_COMPLETED &&
       callback_result != VAULT_RESEAL_ORCHESTRATOR_ERROR)
      return callback_result;
   return guard_result == VAULT_MAINTENANCE_BUSY ? VAULT_RESEAL_ORCHESTRATOR_BUSY
                                                 : VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
}

static vault_reseal_orchestrator_result_t guard_operation_result(int guard_result)
{
   if (guard_result == VAULT_MAINTENANCE_BUSY)
      return VAULT_RESEAL_ORCHESTRATOR_BUSY;
   if (guard_result == VAULT_MAINTENANCE_EPOCH || guard_result == VAULT_MAINTENANCE_INVALID ||
       guard_result == VAULT_MAINTENANCE_WRONG_OWNER)
      return VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
   return VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
}

static vault_reseal_orchestrator_result_t tx_finish(const db2_vault_rewrap_ops_t *db,
                                                    db2_vault_rewrap_tx_t **tx,
                                                    db2_vault_rewrap_result_t edge)
{
   if (edge != DB2_VAULT_REWRAP_OK)
   {
      db->tx_rollback(tx);
      return db_result(edge);
   }
   db2_vault_rewrap_result_t committed = db->tx_commit(tx);
   /* D2a consumes the handle after an actual COMMIT attempt, including an
    * uncertain one, but deliberately retains it for pre-COMMIT phase misuse. */
   if (*tx)
      db->tx_rollback(tx);
   return db_result(committed);
}

static void output_snapshot(vault_reseal_orchestrator_output_t *o,
                            const db2_vault_rewrap_snapshot_t *s)
{
   o->has_state = 1;
   o->state = s->state;
   o->seal_epoch = s->seal_epoch;
   o->fencing_token = s->fencing_token;
   o->old_generation = s->old_generation;
   o->new_generation = s->new_generation;
   memset(o->failure_class, 0, sizeof(o->failure_class));
   if (s->failure_class[0])
   {
      size_t n = strnlen(s->failure_class, sizeof(s->failure_class));
      if (n >= sizeof(o->failure_class))
         n = sizeof(o->failure_class) - 1;
      memcpy(o->failure_class, s->failure_class, n);
   }
}

static vault_reseal_orchestrator_result_t terminal_edge(const vault_reseal_orchestrator_deps_t *d,
                                                        const db2_vault_rewrap_snapshot_t *s,
                                                        const char *failure, int aborting)
{
   db2_vault_rewrap_tx_t *tx = NULL;
   db2_vault_rewrap_result_t r = d->db->tx_begin(&tx);
   if (r == DB2_VAULT_REWRAP_OK)
      r = aborting ? d->db->abort(tx, s->operation_id, s->fencing_token, failure)
                   : d->db->recovery_required(tx, s->operation_id, s->fencing_token, failure);
   /* A known commit is progress, not yet a terminal report. The driver must
    * obtain a fresh durable snapshot and report that terminal row. */
   return tx_finish(d->db, &tx, r);
}

static int receipt_from_snapshot(const db2_vault_rewrap_snapshot_t *s,
                                 vault_tpm2_reseal_receipt_t *r)
{
   return s->has_receipt && vault_reseal_receipt_decode(s->receipt, sizeof(s->receipt), r) == 0;
}

static int cursor_advances(const db2_vault_rewrap_cursor_t *before,
                           const db2_vault_rewrap_cursor_t *after)
{
   if (!before || !after || before->len > sizeof(before->bytes) ||
       after->len > sizeof(after->bytes))
      return 0;
   size_t common = before->len < after->len ? before->len : after->len;
   int cmp = common ? memcmp(after->bytes, before->bytes, common) : 0;
   return cmp > 0 || (cmp == 0 && after->len > before->len);
}

static int stage_callback(const uint8_t old_kek[VAULT_KEK_LEN], void *opaque)
{
   crypto_context_t *c = opaque;
   const db2_vault_rewrap_ops_t *db = c->deps->db;
   const vault_reseal_custody_ops_t *v = c->deps->custody;
   const db2_vault_rewrap_snapshot_t *s = c->snapshot;
   db2_vault_rewrap_tx_t *tx = NULL;
   db2_vault_rewrap_secret_t *secrets = calloc(DB2_VAULT_REWRAP_PAGE_MAX, sizeof(*secrets));
   db2_vault_rewrap_check_t *checks = calloc(DB2_VAULT_REWRAP_PAGE_MAX, sizeof(*checks));
   int64_t after = 0;
   db2_vault_rewrap_cursor_t cursor = {{0}, 0}, next = {{0}, 0};
   db2_vault_rewrap_inventory_summary_t inventory;
   db2_vault_rewrap_result_t r = DB2_VAULT_REWRAP_ERROR;
   int64_t seen_secrets = 0, seen_checks = 0;
   memset(&inventory, 0, sizeof(inventory));
   if (!secrets || !checks)
      goto out;
   r = db->tx_begin(&tx);
   if (r != DB2_VAULT_REWRAP_OK)
   {
      c->result = db_result(r);
      goto out;
   }
   r = db->inventory_summary(tx, s->operation_id, s->fencing_token, &inventory);
   if (r != DB2_VAULT_REWRAP_OK)
      goto rollback;
   if (vault_mutation_budget_bind_inventory(c->budget, (uint64_t)inventory.secret_count,
                                            (uint64_t)inventory.check_count,
                                            inventory.inventory_digest) != 0 ||
       budget_advance(c->budget, c->progress) != 0)
   {
      r = DB2_VAULT_REWRAP_INTEGRITY;
      goto rollback;
   }
   for (;;)
   {
      size_t n = 0;
      r = db->source_secret_page(tx, s->operation_id, s->fencing_token, after,
                                 DB2_VAULT_REWRAP_PAGE_MAX, secrets, DB2_VAULT_REWRAP_PAGE_MAX, &n);
      if (r != DB2_VAULT_REWRAP_OK)
         goto rollback;
      if (n > DB2_VAULT_REWRAP_PAGE_MAX)
      {
         r = DB2_VAULT_REWRAP_INTEGRITY;
         goto rollback;
      }
      if (!n)
         break;
      for (size_t i = 0; i < n; i++)
      {
         if (secrets[i].source_id <= after ||
             v->dek_unwrap(old_kek, secrets[i].wrapped_dek, c->workspace->dek) != 0 ||
             v->dek_wrap(c->workspace->new_kek, c->workspace->dek, c->workspace->wrapped) != 0)
         {
            r = DB2_VAULT_REWRAP_INTEGRITY;
            OPENSSL_cleanse(c->workspace->dek, sizeof(c->workspace->dek));
            goto rollback;
         }
         OPENSSL_cleanse(c->workspace->dek, sizeof(c->workspace->dek));
         r = db->stage_dek(tx, s->operation_id, s->fencing_token, &secrets[i],
                           c->workspace->wrapped);
         OPENSSL_cleanse(c->workspace->wrapped, sizeof(c->workspace->wrapped));
         if (r != DB2_VAULT_REWRAP_OK)
            goto rollback;
         after = secrets[i].source_id;
         if (seen_secrets == INT64_MAX || ++seen_secrets > inventory.secret_count ||
             budget_advance(c->budget, c->progress) != 0)
         {
            r = DB2_VAULT_REWRAP_INTEGRITY;
            goto rollback;
         }
      }
      db2_vault_rewrap_secret_clear(secrets, DB2_VAULT_REWRAP_PAGE_MAX);
   }
   for (;;)
   {
      size_t n = 0;
      r = db->source_check_page(tx, s->operation_id, s->fencing_token, &cursor,
                                DB2_VAULT_REWRAP_PAGE_MAX, checks, DB2_VAULT_REWRAP_PAGE_MAX, &n,
                                &next);
      if (r != DB2_VAULT_REWRAP_OK)
         goto rollback;
      if (n > DB2_VAULT_REWRAP_PAGE_MAX)
      {
         r = DB2_VAULT_REWRAP_INTEGRITY;
         goto rollback;
      }
      if (!n)
         break;
      if (!cursor_advances(&cursor, &next))
      {
         r = DB2_VAULT_REWRAP_INTEGRITY;
         goto rollback;
      }
      for (size_t i = 0; i < n; i++)
      {
         size_t wn = 0;
         if (checks[i].kek_check_len)
         {
            if (checks[i].kek_check_len != VAULT_WRAPPED_DEK_LEN ||
                v->check_verify(old_kek, checks[i].kek_check) != 0 ||
                v->check_wrap(c->workspace->new_kek, c->workspace->wrapped) != 0)
            {
               r = DB2_VAULT_REWRAP_INTEGRITY;
               goto rollback;
            }
            wn = VAULT_WRAPPED_DEK_LEN;
         }
         r = db->stage_check(tx, s->operation_id, s->fencing_token, &checks[i],
                             c->workspace->wrapped, wn);
         OPENSSL_cleanse(c->workspace->wrapped, sizeof(c->workspace->wrapped));
         if (r != DB2_VAULT_REWRAP_OK)
            goto rollback;
         if (seen_checks == INT64_MAX || ++seen_checks > inventory.check_count ||
             budget_advance(c->budget, c->progress) != 0)
         {
            r = DB2_VAULT_REWRAP_INTEGRITY;
            goto rollback;
         }
      }
      cursor = next;
      db2_vault_rewrap_check_clear(checks, DB2_VAULT_REWRAP_PAGE_MAX);
   }
   if (seen_secrets != inventory.secret_count || seen_checks != inventory.check_count)
   {
      r = DB2_VAULT_REWRAP_INTEGRITY;
      goto rollback;
   }
   r = db->stage_finish(tx, s->operation_id, s->fencing_token, &inventory);
   if (r == DB2_VAULT_REWRAP_OK && budget_advance(c->budget, c->progress) != 0)
      r = DB2_VAULT_REWRAP_TRANSIENT;
   c->result = tx_finish(db, &tx, r);
   goto out;
rollback:
   db->tx_rollback(&tx);
   c->result = db_result(r);
out:
   if (secrets)
   {
      db2_vault_rewrap_secret_clear(secrets, DB2_VAULT_REWRAP_PAGE_MAX);
      free(secrets);
   }
   if (checks)
   {
      db2_vault_rewrap_check_clear(checks, DB2_VAULT_REWRAP_PAGE_MAX);
      free(checks);
   }
   db2_vault_rewrap_cursor_clear(&cursor);
   db2_vault_rewrap_cursor_clear(&next);
   OPENSSL_cleanse(&inventory, sizeof(inventory));
   OPENSSL_cleanse(c->workspace->dek, sizeof(c->workspace->dek));
   OPENSSL_cleanse(c->workspace->wrapped, sizeof(c->workspace->wrapped));
   return c->result == VAULT_RESEAL_ORCHESTRATOR_COMPLETED ? VAULT_MAINTENANCE_OK
                                                           : VAULT_MAINTENANCE_ERROR;
}

static int verify_callback(const uint8_t new_kek[VAULT_KEK_LEN], void *opaque)
{
   crypto_context_t *c = opaque;
   const db2_vault_rewrap_ops_t *db = c->deps->db;
   const vault_reseal_custody_ops_t *v = c->deps->custody;
   const db2_vault_rewrap_snapshot_t *s = c->snapshot;
   db2_vault_rewrap_tx_t *tx = NULL;
   db2_vault_rewrap_verify_summary_t sum;
   db2_vault_rewrap_secret_t *secrets = calloc(DB2_VAULT_REWRAP_PAGE_MAX, sizeof(*secrets));
   db2_vault_rewrap_check_t *checks = calloc(DB2_VAULT_REWRAP_PAGE_MAX, sizeof(*checks));
   int64_t after = 0, seen_s = 0, seen_c = 0;
   db2_vault_rewrap_cursor_t cursor = {{0}, 0}, next = {{0}, 0};
   db2_vault_rewrap_result_t r = DB2_VAULT_REWRAP_ERROR;
   memset(&sum, 0, sizeof(sum));
   if (!secrets || !checks)
      goto out;
   r = db->tx_begin(&tx);
   if (r != DB2_VAULT_REWRAP_OK)
   {
      c->result = db_result(r);
      goto out;
   }
   r = db->verify_summary(tx, s->operation_id, s->fencing_token, &sum);
   if (r != DB2_VAULT_REWRAP_OK)
      goto rollback;
   if (vault_mutation_budget_bind_inventory(c->budget, (uint64_t)sum.secret_count,
                                            (uint64_t)sum.check_count, sum.inventory_digest) != 0 ||
       budget_advance(c->budget, c->progress) != 0)
   {
      r = DB2_VAULT_REWRAP_INTEGRITY;
      goto rollback;
   }
   while (seen_s < sum.secret_count)
   {
      size_t n = 0;
      int64_t remaining = sum.secret_count - seen_s;
      int limit =
          remaining < DB2_VAULT_REWRAP_PAGE_MAX ? (int)remaining : DB2_VAULT_REWRAP_PAGE_MAX;
      r = db->verify_secret_page(tx, s->operation_id, s->fencing_token, after, limit, secrets,
                                 DB2_VAULT_REWRAP_PAGE_MAX, &n);
      if (r != DB2_VAULT_REWRAP_OK)
         goto rollback;
      if (!n || n > (size_t)limit || n > (size_t)remaining)
      {
         r = DB2_VAULT_REWRAP_INTEGRITY;
         goto rollback;
      }
      for (size_t i = 0; i < n; i++)
      {
         if (secrets[i].source_id <= after ||
             v->dek_unwrap(new_kek, secrets[i].wrapped_dek, c->workspace->dek) != 0)
         {
            r = DB2_VAULT_REWRAP_INTEGRITY;
            goto rollback;
         }
         OPENSSL_cleanse(c->workspace->dek, sizeof(c->workspace->dek));
         after = secrets[i].source_id;
      }
      seen_s += (int64_t)n;
      if (budget_advance(c->budget, c->progress) != 0)
      {
         r = DB2_VAULT_REWRAP_TRANSIENT;
         goto rollback;
      }
      db2_vault_rewrap_secret_clear(secrets, DB2_VAULT_REWRAP_PAGE_MAX);
   }
   {
      size_t n = 0;
      r = db->verify_secret_page(tx, s->operation_id, s->fencing_token, after, 1, secrets,
                                 DB2_VAULT_REWRAP_PAGE_MAX, &n);
      if (r != DB2_VAULT_REWRAP_OK || n != 0)
      {
         if (r == DB2_VAULT_REWRAP_OK)
            r = DB2_VAULT_REWRAP_INTEGRITY;
         goto rollback;
      }
   }
   while (seen_c < sum.check_count)
   {
      size_t n = 0;
      int64_t remaining = sum.check_count - seen_c;
      int limit =
          remaining < DB2_VAULT_REWRAP_PAGE_MAX ? (int)remaining : DB2_VAULT_REWRAP_PAGE_MAX;
      r = db->verify_check_page(tx, s->operation_id, s->fencing_token, &cursor, limit, checks,
                                DB2_VAULT_REWRAP_PAGE_MAX, &n, &next);
      if (r != DB2_VAULT_REWRAP_OK)
         goto rollback;
      if (!n || n > (size_t)limit || n > (size_t)remaining || !cursor_advances(&cursor, &next))
      {
         r = DB2_VAULT_REWRAP_INTEGRITY;
         goto rollback;
      }
      for (size_t i = 0; i < n; i++)
         if (checks[i].kek_check_len && (checks[i].kek_check_len != VAULT_WRAPPED_DEK_LEN ||
                                         v->check_verify(new_kek, checks[i].kek_check) != 0))
         {
            r = DB2_VAULT_REWRAP_INTEGRITY;
            goto rollback;
         }
      seen_c += (int64_t)n;
      if (budget_advance(c->budget, c->progress) != 0)
      {
         r = DB2_VAULT_REWRAP_TRANSIENT;
         goto rollback;
      }
      cursor = next;
      db2_vault_rewrap_check_clear(checks, DB2_VAULT_REWRAP_PAGE_MAX);
   }
   {
      size_t n = 0;
      db2_vault_rewrap_cursor_t before = cursor;
      r = db->verify_check_page(tx, s->operation_id, s->fencing_token, &cursor, 1, checks,
                                DB2_VAULT_REWRAP_PAGE_MAX, &n, &next);
      if (r != DB2_VAULT_REWRAP_OK || n != 0 || next.len != before.len ||
          (before.len && CRYPTO_memcmp(next.bytes, before.bytes, before.len) != 0))
      {
         if (r == DB2_VAULT_REWRAP_OK)
            r = DB2_VAULT_REWRAP_INTEGRITY;
         db2_vault_rewrap_cursor_clear(&before);
         goto rollback;
      }
      db2_vault_rewrap_cursor_clear(&before);
   }
   if (seen_s != sum.secret_count || seen_c != sum.check_count)
   {
      r = DB2_VAULT_REWRAP_INTEGRITY;
      goto rollback;
   }
   r = db->verify_crypto_ack(tx, s->operation_id, s->fencing_token);
   if (r == DB2_VAULT_REWRAP_OK)
      r = db->complete(tx, s->operation_id, s->fencing_token, sum.receipt_digest,
                       sum.inventory_digest, sum.stage_digest);
   if (r == DB2_VAULT_REWRAP_OK && budget_advance(c->budget, c->progress) != 0)
      r = DB2_VAULT_REWRAP_TRANSIENT;
   c->result = tx_finish(db, &tx, r);
   goto out;
rollback:
   db->tx_rollback(&tx);
   c->result = db_result(r);
out:
   if (secrets)
   {
      db2_vault_rewrap_secret_clear(secrets, DB2_VAULT_REWRAP_PAGE_MAX);
      free(secrets);
   }
   if (checks)
   {
      db2_vault_rewrap_check_clear(checks, DB2_VAULT_REWRAP_PAGE_MAX);
      free(checks);
   }
   db2_vault_rewrap_verify_summary_clear(&sum);
   db2_vault_rewrap_cursor_clear(&cursor);
   db2_vault_rewrap_cursor_clear(&next);
   OPENSSL_cleanse(c->workspace->dek, sizeof(c->workspace->dek));
   return c->result == VAULT_RESEAL_ORCHESTRATOR_COMPLETED ? VAULT_MAINTENANCE_OK
                                                           : VAULT_MAINTENANCE_ERROR;
}

static vault_reseal_orchestrator_result_t
simple_edge(const vault_reseal_orchestrator_deps_t *d, const db2_vault_rewrap_snapshot_t *s,
            db2_vault_rewrap_result_t (*fn)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t))
{
   db2_vault_rewrap_tx_t *tx = NULL;
   db2_vault_rewrap_result_t r = d->db->tx_begin(&tx);
   if (r == DB2_VAULT_REWRAP_OK)
      r = fn(tx, s->operation_id, s->fencing_token);
   return tx_finish(d->db, &tx, r);
}

vault_reseal_orchestrator_result_t
vault_reseal_orchestrator_run(const vault_reseal_orchestrator_request_t *req,
                              const vault_reseal_orchestrator_deps_t *d,
                              vault_reseal_orchestrator_output_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || !request_valid(req) || !deps_valid(d))
      return VAULT_RESEAL_ORCHESTRATOR_INVALID;
   if (!d->custody->supported())
      return VAULT_RESEAL_ORCHESTRATOR_UNSUPPORTED;
   if (vault_mutation_budget_enter(req->budget) != 0)
      return VAULT_RESEAL_ORCHESTRATOR_INVALID;

   protected_arena_t sa = {0}, wa = {0};
   void *guard = NULL;
   uint64_t progress = 0;
   db2_vault_rewrap_state_t last_state = DB2_VAULT_REWRAP_PREPARING;
   int last_state_valid = 0;
   vault_reseal_orchestrator_result_t result = VAULT_RESEAL_ORCHESTRATOR_ERROR;
   if (arena_new(req->provider_secret_len + 1, &sa) != 0 ||
       arena_new(sizeof(secret_workspace_t), &wa) != 0)
      goto done;
   memcpy(sa.p, req->provider_secret, req->provider_secret_len);
   ((char *)sa.p)[req->provider_secret_len] = 0;
   char *secret = sa.p;
   secret_workspace_t *workspace = wa.p;
   int gr = d->custody->guard_begin(&guard);
   if (gr != VAULT_MAINTENANCE_OK)
   {
      result = gr == VAULT_MAINTENANCE_BUSY ? VAULT_RESEAL_ORCHESTRATOR_BUSY
                                            : VAULT_RESEAL_ORCHESTRATOR_ERROR;
      goto done;
   }

   int starting = req->mode == VAULT_RESEAL_ORCHESTRATOR_START;
   for (unsigned edges = 0; edges < VAULT_RESEAL_ORCHESTRATOR_EDGE_MAX; edges++)
   {
      db2_vault_rewrap_snapshot_t s;
      memset(&s, 0, sizeof(s));
      db2_vault_rewrap_result_t sr = d->db->snapshot(req->operation_id, &s);
      if (sr == DB2_VAULT_REWRAP_NOT_FOUND && starting)
      {
         uint64_t gen = 0;
         int cr = d->custody->nv_generation(secret, &gen);
         if (cr != VAULT_TPM2_RESEAL_OK || gen >= (uint64_t)INT64_MAX)
         {
            result = cr == VAULT_TPM2_RESEAL_OK ? VAULT_RESEAL_ORCHESTRATOR_INTEGRITY
                                                : custody_result(cr);
            db2_vault_rewrap_snapshot_clear(&s);
            break;
         }
         db2_vault_rewrap_tx_t *tx = NULL;
         int64_t epoch = 0, fence = 0;
         db2_vault_rewrap_state_t state = DB2_VAULT_REWRAP_PREPARING;
         sr = d->db->tx_begin(&tx);
         if (sr == DB2_VAULT_REWRAP_OK)
            sr = d->db->begin(tx, req->actor, req->request_id, req->operation_id, (int64_t)gen,
                              (int64_t)gen + 1, &epoch, &fence, &state);
         db2_vault_rewrap_result_t begin_result = sr;
         result = tx_finish(d->db, &tx, sr);
         if (begin_result == DB2_VAULT_REWRAP_CONFLICT ||
             begin_result == DB2_VAULT_REWRAP_INTEGRITY || begin_result == DB2_VAULT_REWRAP_INVALID)
            result = VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
         db2_vault_rewrap_snapshot_clear(&s);
         if (result != VAULT_RESEAL_ORCHESTRATOR_COMPLETED)
            break;
         starting = 0;
         continue;
      }
      if (sr != DB2_VAULT_REWRAP_OK)
      {
         result =
             sr == DB2_VAULT_REWRAP_NOT_FOUND ? VAULT_RESEAL_ORCHESTRATOR_INVALID : db_result(sr);
         db2_vault_rewrap_snapshot_clear(&s);
         break;
      }
      if (s.has_inventory &&
          vault_mutation_budget_bind_inventory(req->budget, (uint64_t)s.secret_count,
                                               (uint64_t)s.check_count, s.inventory_digest) != 0)
      {
         result = VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
         db2_vault_rewrap_snapshot_clear(&s);
         break;
      }
      if (!last_state_valid || s.state != last_state)
      {
         if (budget_advance(req->budget, &progress) != 0)
         {
            result = VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
            db2_vault_rewrap_snapshot_clear(&s);
            break;
         }
         last_state = s.state;
         last_state_valid = 1;
      }
      if (starting)
      {
         db2_vault_rewrap_tx_t *tx = NULL;
         int64_t epoch = 0, fence = 0;
         db2_vault_rewrap_state_t state = DB2_VAULT_REWRAP_PREPARING;
         sr = d->db->tx_begin(&tx);
         if (sr == DB2_VAULT_REWRAP_OK)
            sr = d->db->begin(tx, req->actor, req->request_id, req->operation_id, s.old_generation,
                              s.new_generation, &epoch, &fence, &state);
         db2_vault_rewrap_result_t begin_result = sr;
         result = tx_finish(d->db, &tx, sr);
         if (begin_result == DB2_VAULT_REWRAP_CONFLICT ||
             begin_result == DB2_VAULT_REWRAP_INTEGRITY || begin_result == DB2_VAULT_REWRAP_INVALID)
            result = VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
         db2_vault_rewrap_snapshot_clear(&s);
         starting = 0;
         if (result != VAULT_RESEAL_ORCHESTRATOR_COMPLETED)
            break;
         continue;
      }
      output_snapshot(out, &s);
      if (s.state == DB2_VAULT_REWRAP_RECOVERY_REQUIRED)
      {
         result = VAULT_RESEAL_ORCHESTRATOR_RECOVERY_REQUIRED;
         db2_vault_rewrap_snapshot_clear(&s);
         break;
      }
      int sync_result = VAULT_MAINTENANCE_OK;
      if (s.state != DB2_VAULT_REWRAP_COMPLETED && s.state != DB2_VAULT_REWRAP_ABORTED)
         sync_result = d->custody->guard_sync_epoch(guard, (uint64_t)s.seal_epoch);
      if (sync_result != VAULT_MAINTENANCE_OK)
      {
         if (sync_result == VAULT_MAINTENANCE_BUSY)
            result = VAULT_RESEAL_ORCHESTRATOR_BUSY;
         else if (sync_result == VAULT_MAINTENANCE_EPOCH ||
                  sync_result == VAULT_MAINTENANCE_INVALID ||
                  sync_result == VAULT_MAINTENANCE_WRONG_OWNER)
            result = VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
         else
            result = VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
         db2_vault_rewrap_snapshot_clear(&s);
         break;
      }

      vault_tpm2_reseal_receipt_t receipt;
      vault_tpm2_reseal_status_t status = VAULT_TPM2_RESEAL_CORRUPT;
      memset(&receipt, 0, sizeof(receipt));
      int cr;
      if (s.state == DB2_VAULT_REWRAP_PREPARING ||
          (s.state == DB2_VAULT_REWRAP_ABORTED && !s.has_receipt))
         cr = d->custody->discover(s.operation_id, (uint64_t)s.old_generation, secret, &receipt,
                                   &status);
      else if (receipt_from_snapshot(&s, &receipt))
         cr = d->custody->status(&receipt, secret, &status);
      else
         cr = VAULT_TPM2_RESEAL_INTEGRITY;
      if (cr != VAULT_TPM2_RESEAL_OK)
      {
         result = custody_result(cr);
         if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY &&
             s.state != DB2_VAULT_REWRAP_COMPLETED && s.state != DB2_VAULT_REWRAP_ABORTED)
            result = terminal_edge(d, &s, "custody_integrity", 0);
         OPENSSL_cleanse(&receipt, sizeof(receipt));
         db2_vault_rewrap_snapshot_clear(&s);
         if (result == VAULT_RESEAL_ORCHESTRATOR_COMPLETED)
         {
            result = VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
            continue;
         }
         break;
      }

      if (s.state == DB2_VAULT_REWRAP_ABORTED)
      {
         result = status == VAULT_TPM2_RESEAL_ABSENT ? VAULT_RESEAL_ORCHESTRATOR_ABORTED
                                                     : VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
      }
      else if (s.state == DB2_VAULT_REWRAP_COMPLETED)
      {
         result = status == VAULT_TPM2_RESEAL_INSTALLED || status == VAULT_TPM2_RESEAL_CLEANED
                      ? VAULT_RESEAL_ORCHESTRATOR_COMPLETED
                      : VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
      }
      else if (s.state == DB2_VAULT_REWRAP_PREPARING &&
               (status == VAULT_TPM2_RESEAL_ABSENT || status == VAULT_TPM2_RESEAL_PREPARED))
      {
         if (status == VAULT_TPM2_RESEAL_ABSENT)
         {
            if (d->custody->random(workspace->new_kek, VAULT_KEK_LEN) != 0)
               result = VAULT_RESEAL_ORCHESTRATOR_ERROR;
            else
            {
               workspace->has_new_kek = 1;
               cr = d->custody->prepare(s.operation_id, (uint64_t)s.old_generation,
                                        workspace->new_kek, secret, &receipt);
               result = cr == VAULT_TPM2_RESEAL_OK ? VAULT_RESEAL_ORCHESTRATOR_COMPLETED
                                                   : custody_result(cr);
               if (cr != VAULT_TPM2_RESEAL_OK)
               {
                  vault_tpm2_reseal_status_t ds;
                  vault_tpm2_reseal_receipt_t dr;
                  memset(&dr, 0, sizeof(dr));
                  int dc = d->custody->discover(s.operation_id, (uint64_t)s.old_generation, secret,
                                                &dr, &ds);
                  if (dc == VAULT_TPM2_RESEAL_OK && ds == VAULT_TPM2_RESEAL_PREPARED)
                  {
                     /* The lost prepare response may have exposed an older
                      * exact-op artifact. Adopt only the KEK authenticated by
                      * that discovered receipt, never the retained random key. */
                     int rr = d->custody->recover_kek(&dr, secret, workspace->new_kek);
                     workspace->has_new_kek = rr == VAULT_TPM2_RESEAL_OK;
                     if (workspace->has_new_kek)
                     {
                        receipt = dr;
                        result = VAULT_RESEAL_ORCHESTRATOR_COMPLETED;
                     }
                     else
                        result = custody_result(rr);
                  }
                  else if (dc == VAULT_TPM2_RESEAL_OK && ds != VAULT_TPM2_RESEAL_ABSENT)
                  {
                     result = terminal_edge(d, &s, "generation_conflict", 0);
                     OPENSSL_cleanse(&dr, sizeof(dr));
                     goto iteration_done;
                  }
                  else if (dc == VAULT_TPM2_RESEAL_INTEGRITY)
                     result = VAULT_RESEAL_ORCHESTRATOR_INTEGRITY;
                  else if (dc != VAULT_TPM2_RESEAL_OK)
                     result = custody_result(dc);
                  OPENSSL_cleanse(&dr, sizeof(dr));
               }
            }
         }
         else
         {
            cr = d->custody->recover_kek(&receipt, secret, workspace->new_kek);
            workspace->has_new_kek = cr == VAULT_TPM2_RESEAL_OK;
            result = cr == VAULT_TPM2_RESEAL_OK ? VAULT_RESEAL_ORCHESTRATOR_COMPLETED
                                                : custody_result(cr);
         }
         if (result == VAULT_RESEAL_ORCHESTRATOR_COMPLETED)
         {
            uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
            if (vault_reseal_receipt_encode(&receipt, wire) != 0)
               result = terminal_edge(d, &s, "receipt_mismatch", 0);
            else
            {
               db2_vault_rewrap_tx_t *tx = NULL;
               sr = d->db->tx_begin(&tx);
               if (sr == DB2_VAULT_REWRAP_OK)
                  sr = d->db->record_prepared(tx, s.operation_id, s.fencing_token, s.old_generation,
                                              s.new_generation, wire);
               result = tx_finish(d->db, &tx, sr);
            }
            OPENSSL_cleanse(wire, sizeof(wire));
         }
         if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY ||
             result == VAULT_RESEAL_ORCHESTRATOR_INVALID)
            result = terminal_edge(d, &s, "receipt_mismatch", 0);
      }
      else if (s.state == DB2_VAULT_REWRAP_CUSTODY_PREPARED && status == VAULT_TPM2_RESEAL_PREPARED)
      {
         if (!workspace->has_new_kek)
         {
            cr = d->custody->recover_kek(&receipt, secret, workspace->new_kek);
            workspace->has_new_kek = cr == VAULT_TPM2_RESEAL_OK;
         }
         if (!workspace->has_new_kek)
         {
            result = custody_result(cr);
            if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY)
               result = terminal_edge(d, &s, "custody_integrity", 0);
         }
         else
         {
            int unseal_result = d->custody->guard_unseal(guard, secret, req->provider_secret_len);
            if (unseal_result != VAULT_MAINTENANCE_OK)
               result = guard_operation_result(unseal_result);
            else
            {
               crypto_context_t ctx = {d,           &s,        workspace,
                                       req->budget, &progress, VAULT_RESEAL_ORCHESTRATOR_ERROR};
               int cb = d->custody->guard_with_active_kek(guard, stage_callback, &ctx);
               result = guard_callback_result(cb, ctx.result);
               if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY)
                  result = terminal_edge(d, &s, "source_integrity", 0);
            }
         }
      }
      else if ((s.state == DB2_VAULT_REWRAP_CUSTODY_PREPARED ||
                s.state == DB2_VAULT_REWRAP_WRAPS_STAGED) &&
               status == VAULT_TPM2_RESEAL_ABSENT)
      {
         int ar = d->custody->abort(&receipt, secret);
         if (ar != VAULT_TPM2_RESEAL_OK)
            result = custody_result(ar);
         else
         {
            vault_tpm2_reseal_receipt_t absent_receipt;
            vault_tpm2_reseal_status_t after_abort = VAULT_TPM2_RESEAL_CORRUPT;
            uint64_t gen = UINT64_MAX;
            memset(&absent_receipt, 0, sizeof(absent_receipt));
            int dr = d->custody->discover(s.operation_id, (uint64_t)s.old_generation, secret,
                                          &absent_receipt, &after_abort);
            if (dr == VAULT_TPM2_RESEAL_OK && after_abort == VAULT_TPM2_RESEAL_ABSENT)
            {
               int nr = d->custody->nv_generation(secret, &gen);
               if (nr != VAULT_TPM2_RESEAL_OK)
                  result = custody_result(nr);
               else if (gen != (uint64_t)s.old_generation)
                  result = terminal_edge(d, &s, "generation_conflict", 0);
               else
                  result = terminal_edge(d, &s, "prepared_missing", 1);
            }
            else if (dr == VAULT_TPM2_RESEAL_OK || dr == VAULT_TPM2_RESEAL_INTEGRITY)
               result = terminal_edge(d, &s, "custody_integrity", 0);
            else
               result = custody_result(dr);
            OPENSSL_cleanse(&absent_receipt, sizeof(absent_receipt));
         }
      }
      else if (s.state == DB2_VAULT_REWRAP_WRAPS_STAGED && status == VAULT_TPM2_RESEAL_PREPARED)
      {
         result = simple_edge(d, &s, d->db->mark_committing);
         if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY ||
             result == VAULT_RESEAL_ORCHESTRATOR_INVALID)
            result = terminal_edge(d, &s, "commit_intent_integrity", 0);
      }
      else if (s.state == DB2_VAULT_REWRAP_RESEAL_COMMITTING &&
               (status == VAULT_TPM2_RESEAL_PREPARED || status == VAULT_TPM2_RESEAL_NV_ADVANCED ||
                status == VAULT_TPM2_RESEAL_INSTALLED))
      {
         vault_tpm2_reseal_status_t installed = VAULT_TPM2_RESEAL_CORRUPT;
         cr = d->custody->commit(&receipt, secret, &installed);
         if (cr != VAULT_TPM2_RESEAL_OK)
         {
            result = custody_result(cr);
            if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY)
               result = terminal_edge(d, &s, "custody_integrity", 0);
         }
         else if (installed != VAULT_TPM2_RESEAL_INSTALLED)
            result = terminal_edge(d, &s, "custody_integrity", 0);
         else
         {
            db2_vault_rewrap_tx_t *tx = NULL;
            sr = d->db->tx_begin(&tx);
            if (sr == DB2_VAULT_REWRAP_OK)
               sr = d->db->mark_resealed(tx, s.operation_id, s.fencing_token, s.receipt_digest);
            result = tx_finish(d->db, &tx, sr);
            if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY ||
                result == VAULT_RESEAL_ORCHESTRATOR_INVALID)
               result = terminal_edge(d, &s, "receipt_mismatch", 0);
         }
      }
      else if (s.state == DB2_VAULT_REWRAP_RESEALED && status == VAULT_TPM2_RESEAL_INSTALLED)
      {
         result = simple_edge(d, &s, d->db->promote);
         if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY ||
             result == VAULT_RESEAL_ORCHESTRATOR_INVALID)
            result = terminal_edge(d, &s, "promotion_integrity", 0);
      }
      else if (s.state == DB2_VAULT_REWRAP_PROMOTED && status == VAULT_TPM2_RESEAL_INSTALLED)
      {
         int seal_result = d->custody->guard_seal(guard);
         if (seal_result != VAULT_MAINTENANCE_OK)
            result = guard_operation_result(seal_result);
         else
         {
            int unseal_result = d->custody->guard_unseal(guard, secret, req->provider_secret_len);
            if (unseal_result != VAULT_MAINTENANCE_OK)
               result = guard_operation_result(unseal_result);
            else
            {
               crypto_context_t ctx = {d,           &s,        workspace,
                                       req->budget, &progress, VAULT_RESEAL_ORCHESTRATOR_ERROR};
               int cb = d->custody->guard_with_active_kek(guard, verify_callback, &ctx);
               result = guard_callback_result(cb, ctx.result);
               if (result == VAULT_RESEAL_ORCHESTRATOR_INTEGRITY)
                  result = terminal_edge(d, &s, "verification_integrity", 0);
            }
         }
      }
      else
         result = terminal_edge(d, &s, "generation_conflict", 0);

   iteration_done:
      OPENSSL_cleanse(&receipt, sizeof(receipt));
      db2_vault_rewrap_snapshot_clear(&s);
      if (out->has_state && out->state == DB2_VAULT_REWRAP_COMPLETED &&
          result == VAULT_RESEAL_ORCHESTRATOR_COMPLETED)
         break;
      if (result != VAULT_RESEAL_ORCHESTRATOR_COMPLETED)
         break;
      result = VAULT_RESEAL_ORCHESTRATOR_SAFE_RETRY;
   }

   if (guard)
   {
      if (vault_mutation_budget_begin_cleanup(req->budget) != 0)
         result = VAULT_RESEAL_ORCHESTRATOR_ERROR;
      if (d->custody->guard_seal(guard) != VAULT_MAINTENANCE_OK)
         result = VAULT_RESEAL_ORCHESTRATOR_ERROR;
      if (d->custody->guard_end(&guard) != VAULT_MAINTENANCE_OK)
         result = VAULT_RESEAL_ORCHESTRATOR_ERROR;
   }
done:
   arena_free(&wa);
   arena_free(&sa);
   if (vault_mutation_budget_leave(req->budget) != 0)
      result = VAULT_RESEAL_ORCHESTRATOR_ERROR;
   return result;
}

static int default_supported(void)
{
#if defined(WITH_TPM2) && defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   return 1;
#else
   return 0;
#endif
}
static int default_guard_begin(void **g)
{
   return vault_maintenance_guard_begin((vault_maintenance_guard_t **)g);
}
static int default_guard_sync(void *g, uint64_t e)
{
   return vault_maintenance_guard_sync_primary_epoch((vault_maintenance_guard_t *)g, e);
}
static int default_guard_unseal(void *g, const void *p, size_t n)
{
   return vault_maintenance_guard_unseal((vault_maintenance_guard_t *)g, p, n);
}
static int default_guard_seal(void *g)
{
   return vault_maintenance_guard_seal((vault_maintenance_guard_t *)g);
}
static int default_guard_with(void *g, vault_maintenance_kek_fn f, void *c)
{
   return vault_maintenance_guard_with_active_kek((vault_maintenance_guard_t *)g, f, c);
}
static int default_guard_end(void **g)
{
   return vault_maintenance_guard_end((vault_maintenance_guard_t **)g);
}

const vault_reseal_custody_ops_t vault_reseal_custody_default_ops = {
    .supported = default_supported,
    .nv_generation = vault_custody_tpm2_nv_generation,
    .prepare = vault_custody_tpm2_reseal_prepare,
    .discover = vault_custody_tpm2_reseal_discover,
    .recover_kek = vault_custody_tpm2_reseal_recover_kek,
    .status = vault_custody_tpm2_reseal_status,
    .commit = vault_custody_tpm2_reseal_commit,
    .abort = vault_custody_tpm2_reseal_abort,
    .guard_begin = default_guard_begin,
    .guard_sync_epoch = default_guard_sync,
    .guard_unseal = default_guard_unseal,
    .guard_seal = default_guard_seal,
    .guard_with_active_kek = default_guard_with,
    .guard_end = default_guard_end,
    .random = vault_crypto_random,
    .dek_wrap = vault_dek_wrap,
    .dek_unwrap = vault_dek_unwrap,
    .check_wrap = vault_kek_check_wrap,
    .check_verify = vault_kek_check_verify,
};

const vault_reseal_orchestrator_deps_t vault_reseal_orchestrator_default_deps = {
    .db = &db2_vault_rewrap_default_ops,
    .custody = &vault_reseal_custody_default_ops,
};
