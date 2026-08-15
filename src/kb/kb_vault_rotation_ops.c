#include "kb_vault_rotation_ops.h"

#include "modules/db2/c/db2_tenant.h"
#include "kb_vault_policy.h"
#include "kb_vault_rotation.h"
#include "vault_crypto.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct
{
   unsigned char secret[DB2_VAULT_ROTATION_SECRET_MAX];
   unsigned char kek[VAULT_KEK_LEN];
   unsigned char dek[VAULT_DEK_LEN];
} rotation_secret_arena_t;

typedef struct
{
   const kb_principal_t *caller;
   int64_t team_id;
   int64_t rotation_id;
   const char *owner;
   int64_t token;
   int ttl_seconds;
} rotation_lease_ctx_t;

static pthread_mutex_t g_provider_mu = PTHREAD_MUTEX_INITIALIZER;
static kb_vault_rotation_provider_t g_provider;
static void *g_provider_ctx;
static int g_provider_registered;
static int g_provider_started;

static rotation_secret_arena_t *arena_new(size_t *mapped)
{
#if defined(__linux__) && defined(MADV_DONTDUMP)
   long page = sysconf(_SC_PAGESIZE);
   if (page <= 0)
      return NULL;
   size_t n = (sizeof(rotation_secret_arena_t) + (size_t)page - 1) & ~((size_t)page - 1);
   void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (p == MAP_FAILED)
      return NULL;
   if (mlock(p, n) != 0 || madvise(p, n, MADV_DONTDUMP) != 0)
   {
      OPENSSL_cleanse(p, n);
      (void)munlock(p, n);
      (void)munmap(p, n);
      return NULL;
   }
   *mapped = n;
   return p;
#else
   (void)mapped;
   return NULL;
#endif
}

static void arena_free(rotation_secret_arena_t *arena, size_t mapped)
{
   if (!arena)
      return;
   OPENSSL_cleanse(arena, mapped);
#if defined(__linux__)
   (void)munlock(arena, mapped);
   (void)munmap(arena, mapped);
#else
   (void)mapped;
#endif
}

static int provider_complete(const kb_vault_rotation_provider_t *p)
{
   return p && p->resolve_current && p->provision && p->probe && p->revoke && p->reconcile;
}

int kb_vault_rotation_ops_register(const kb_vault_rotation_provider_t *provider, void *provider_ctx)
{
   if (!provider_complete(provider))
      return -1;
   pthread_mutex_lock(&g_provider_mu);
   int rc = -1;
   if (!g_provider_registered && !g_provider_started)
   {
      g_provider = *provider;
      g_provider_ctx = provider_ctx;
      g_provider_registered = 1;
      rc = 0;
   }
   pthread_mutex_unlock(&g_provider_mu);
   return rc;
}

static int provider_snapshot(kb_vault_rotation_provider_t *provider, void **provider_ctx)
{
   pthread_mutex_lock(&g_provider_mu);
   int rc = -1;
   if (g_provider_registered)
   {
      g_provider_started = 1;
      *provider = g_provider;
      *provider_ctx = g_provider_ctx;
      rc = 0;
   }
   pthread_mutex_unlock(&g_provider_mu);
   return rc;
}

static int bounded_string(const char *value, size_t cap)
{
   return value && cap > 1 && value[0] && memchr(value, '\0', cap) != NULL;
}

static int scope_begin(const kb_principal_t *caller, int64_t team_id, char actor[576])
{
   return caller && kb_identity_key(caller, actor, 576) == 0 &&
                  db2_tenant_scope_begin(caller, team_id) == 0
              ? 0
              : -1;
}

static int scope_end(int rc)
{
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit() == 0 ? 0 : -1;
}

static int load_row(const kb_principal_t *caller, int64_t team_id, int64_t id,
                    db2_vault_rotation_row_t *row, char actor[576])
{
   if (scope_begin(caller, team_id, actor) != 0)
      return -1;
   return scope_end(db2_vault_rotation_get(id, row));
}

static int operation_key(int64_t id, const char *step, char out[128])
{
   int n = snprintf(out, 128, "aimee:p7:rotation:%lld:%s", (long long)id, step);
   return n > 0 && n < 128 ? 0 : -1;
}

static int claim(const kb_principal_t *caller, int64_t team_id, int64_t id, const char *state,
                 const char *owner, int ttl, int64_t *token, char actor[576])
{
   if (scope_begin(caller, team_id, actor) != 0)
      return -1;
   return scope_end(db2_vault_rotation_claim(actor, id, state, owner, ttl, token));
}

static void release_best_effort(const kb_principal_t *caller, int64_t team_id, int64_t id,
                                const char *owner, int64_t token)
{
   char actor[576];
   if (scope_begin(caller, team_id, actor) == 0)
      (void)scope_end(db2_vault_rotation_release(actor, id, owner, token));
}

int kb_vault_rotation_ops_heartbeat(const kb_principal_t *caller, int64_t team_id,
                                    int64_t rotation_id, const char *owner, int64_t token,
                                    int ttl_seconds)
{
   char actor[576];
   if (scope_begin(caller, team_id, actor) != 0)
      return -1;
   return scope_end(db2_vault_rotation_heartbeat(actor, rotation_id, owner, token, ttl_seconds));
}

static int lease_heartbeat(void *opaque)
{
   rotation_lease_ctx_t *lease = opaque;
   return kb_vault_rotation_ops_heartbeat(lease->caller, lease->team_id, lease->rotation_id,
                                          lease->owner, lease->token, lease->ttl_seconds);
}

static kb_vault_rotation_lease_t lease_for(rotation_lease_ctx_t *ctx)
{
   kb_vault_rotation_lease_t lease = {
       .ttl_seconds = ctx->ttl_seconds, .heartbeat = lease_heartbeat, .heartbeat_ctx = ctx};
   return lease;
}

static int build_envelope(const db2_vault_rotation_row_t *row, const unsigned char *secret,
                          size_t secret_len, rotation_secret_arena_t *arena,
                          db2_vault_rotation_envelope_t *envelope)
{
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   if (vault_aad_build_v2(row->principal, row->agent, row->cred, row->to_version, aad, sizeof(aad),
                          &aad_len) != 0 ||
       secret_len > sizeof(envelope->ciphertext) || vault_server_kek(arena->kek) != 0 ||
       vault_crypto_random(arena->dek, sizeof(arena->dek)) != 0)
      return -1;
   memset(envelope, 0, sizeof(*envelope));
   envelope->version = row->to_version;
   envelope->ciphertext_len = secret_len;
   if (vault_secret_encrypt(arena->dek, aad, aad_len, secret, secret_len, envelope->nonce,
                            envelope->ciphertext, envelope->tag) != 0 ||
       vault_dek_wrap(arena->kek, arena->dek, envelope->wrapped_dek) != 0)
   {
      OPENSSL_cleanse(envelope, sizeof(*envelope));
      return -1;
   }
   return 0;
}

static int decrypt_envelope(const db2_vault_rotation_row_t *row,
                            const db2_vault_rotation_envelope_t *envelope,
                            rotation_secret_arena_t *arena)
{
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   if (vault_aad_build_v2(row->principal, row->agent, row->cred, envelope->version, aad,
                          sizeof(aad), &aad_len) != 0 ||
       envelope->version != row->to_version || envelope->ciphertext_len > sizeof(arena->secret) ||
       vault_server_kek(arena->kek) != 0 ||
       vault_dek_unwrap(arena->kek, envelope->wrapped_dek, arena->dek) != 0)
      return -1;
   int rc = vault_secret_decrypt(arena->dek, aad, aad_len, envelope->nonce, envelope->ciphertext,
                                 envelope->ciphertext_len, envelope->tag, arena->secret);
   if (rc != 0 && vault_aad_build_v1_safe(row->principal, row->agent, row->cred, envelope->version,
                                          aad, sizeof(aad), &aad_len) == 0)
      rc = vault_secret_decrypt(arena->dek, aad, aad_len, envelope->nonce, envelope->ciphertext,
                                envelope->ciphertext_len, envelope->tag, arena->secret);
   return rc;
}

static int fail_claimed(const kb_principal_t *caller, int64_t team_id, int64_t id,
                        const char *owner, int64_t token, const char *state, const char *phase)
{
   char actor[576];
   if (scope_begin(caller, team_id, actor) != 0)
      return -1;
   return scope_end(db2_vault_rotation_fail_claimed(actor, id, owner, token, state, phase,
                                                    "definite provider failure"));
}

static int durable_failure(const kb_principal_t *caller, int64_t team_id, int64_t id,
                           const char *owner, int64_t token, const char *state, const char *phase)
{
   return fail_claimed(caller, team_id, id, owner, token, state, phase) == 0
              ? KB_VAULT_OP_DEFINITE_FAILURE
              : KB_VAULT_OP_RETRY;
}

static int provision_step(const kb_principal_t *caller, int64_t team_id,
                          const db2_vault_rotation_row_t *row, const char *owner, int ttl,
                          const kb_vault_rotation_provider_t *provider, void *ctx)
{
   char actor[576], op[128], old_ref[DB2_VAULT_ROTATION_REF_MAX + 1] = "";
   char new_ref[DB2_VAULT_ROTATION_REF_MAX + 1] = "";
   int64_t token = 0;
   if (operation_key(row->id, "provision", op) != 0 ||
       claim(caller, team_id, row->id, "provision", owner, ttl, &token, actor) != 0)
      return KB_VAULT_OP_RETRY;
   rotation_lease_ctx_t lease_ctx = {caller, team_id, row->id, owner, token, ttl};
   kb_vault_rotation_lease_t lease = lease_for(&lease_ctx);
   int rc = provider->resolve_current(ctx, op, row, &lease, old_ref, sizeof(old_ref));
   if (rc != KB_VAULT_OP_OK || !bounded_string(old_ref, sizeof(old_ref)))
   {
      if (rc == KB_VAULT_OP_UNCERTAIN)
      {
         release_best_effort(caller, team_id, row->id, owner, token);
         return KB_VAULT_OP_RETRY;
      }
      return durable_failure(caller, team_id, row->id, owner, token, "provision", "provision");
   }
   if (scope_begin(caller, team_id, actor) != 0 ||
       scope_end(db2_vault_rotation_checkpoint_old_ref(actor, row->id, owner, token, old_ref)) != 0)
      return KB_VAULT_OP_RETRY;

   size_t mapped = 0, secret_len = 0;
   rotation_secret_arena_t *arena = arena_new(&mapped);
   db2_vault_rotation_envelope_t envelope;
   int reconciled = 0;
   if (!arena)
   {
      release_best_effort(caller, team_id, row->id, owner, token);
      return KB_VAULT_OP_RETRY;
   }
   rc = provider->provision(ctx, op, row, &lease, arena->secret, sizeof(arena->secret), &secret_len,
                            new_ref, sizeof(new_ref), &reconciled);
   if (rc == KB_VAULT_OP_UNCERTAIN)
   {
      arena_free(arena, mapped);
      release_best_effort(caller, team_id, row->id, owner, token);
      return KB_VAULT_OP_RETRY;
   }
   if (rc != KB_VAULT_OP_OK || !secret_len || secret_len > sizeof(arena->secret) ||
       !bounded_string(new_ref, sizeof(new_ref)) || !reconciled ||
       build_envelope(row, arena->secret, secret_len, arena, &envelope) != 0)
   {
      arena_free(arena, mapped);
      return durable_failure(caller, team_id, row->id, owner, token, "provision", "provision");
   }
   arena_free(arena, mapped);
   if (scope_begin(caller, team_id, actor) != 0)
   {
      OPENSSL_cleanse(&envelope, sizeof(envelope));
      return KB_VAULT_OP_RETRY;
   }
   rc = db2_vault_rotation_stage_claimed(actor, row->id, owner, token, new_ref, &envelope);
   OPENSSL_cleanse(&envelope, sizeof(envelope));
   (void)scope_end(rc);
   return KB_VAULT_OP_RETRY;
}

static int probe_step(const kb_principal_t *caller, int64_t team_id,
                      const db2_vault_rotation_row_t *row, const char *owner, int ttl,
                      const kb_vault_rotation_provider_t *provider, void *ctx)
{
   char actor[576], op[128];
   int64_t token = 0;
   if (operation_key(row->id, "probe", op) != 0 ||
       claim(caller, team_id, row->id, "staged", owner, ttl, &token, actor) != 0)
      return KB_VAULT_OP_RETRY;
   rotation_lease_ctx_t lease_ctx = {caller, team_id, row->id, owner, token, ttl};
   kb_vault_rotation_lease_t lease = lease_for(&lease_ctx);
   db2_vault_rotation_envelope_t envelope;
   if (scope_begin(caller, team_id, actor) != 0 ||
       scope_end(db2_vault_rotation_probe_admit(actor, row->id, owner, token, op, &envelope)) != 0)
      return KB_VAULT_OP_RETRY;
   size_t mapped = 0;
   rotation_secret_arena_t *arena = arena_new(&mapped);
   if (!arena)
   {
      OPENSSL_cleanse(&envelope, sizeof(envelope));
      release_best_effort(caller, team_id, row->id, owner, token);
      return KB_VAULT_OP_RETRY;
   }
   if (decrypt_envelope(row, &envelope, arena) != 0)
   {
      arena_free(arena, mapped);
      OPENSSL_cleanse(&envelope, sizeof(envelope));
      return durable_failure(caller, team_id, row->id, owner, token, "staged", "probe");
   }
   int rc = provider->probe(ctx, op, row, &lease, arena->secret, envelope.ciphertext_len);
   arena_free(arena, mapped);
   OPENSSL_cleanse(&envelope, sizeof(envelope));
   if (rc == KB_VAULT_OP_UNCERTAIN)
   {
      release_best_effort(caller, team_id, row->id, owner, token);
      return KB_VAULT_OP_RETRY;
   }
   if (rc != KB_VAULT_OP_OK)
   {
      return durable_failure(caller, team_id, row->id, owner, token, "staged", "probe");
   }
   if (scope_begin(caller, team_id, actor) != 0)
      return KB_VAULT_OP_RETRY;
   rc = db2_vault_rotation_transition_claimed(actor, row->id, owner, token, "staged", "probed", "");
   (void)scope_end(rc);
   return KB_VAULT_OP_RETRY;
}

static int revoke_step(const kb_principal_t *caller, int64_t team_id,
                       const db2_vault_rotation_row_t *row, const char *owner, int ttl,
                       const kb_vault_rotation_provider_t *provider, void *ctx)
{
   char actor[576], op[128], receipt[DB2_VAULT_ROTATION_REF_MAX + 1] = "";
   int64_t token = 0;
   if (!row->old_vendor_ref[0] || operation_key(row->id, "revoke-old", op) != 0 ||
       claim(caller, team_id, row->id, "activated", owner, ttl, &token, actor) != 0)
      return KB_VAULT_OP_RETRY;
   rotation_lease_ctx_t lease_ctx = {caller, team_id, row->id, owner, token, ttl};
   kb_vault_rotation_lease_t lease = lease_for(&lease_ctx);
   int rc = provider->revoke(ctx, op, row, &lease, row->old_vendor_ref, receipt, sizeof(receipt));
   if (rc != KB_VAULT_OP_OK || !bounded_string(receipt, sizeof(receipt)))
   {
      release_best_effort(caller, team_id, row->id, owner, token);
      return KB_VAULT_OP_RETRY;
   }
   if (scope_begin(caller, team_id, actor) != 0)
      return KB_VAULT_OP_RETRY;
   rc = db2_vault_rotation_transition_claimed(actor, row->id, owner, token, "activated", "revoked",
                                              receipt);
   (void)scope_end(rc);
   return KB_VAULT_OP_RETRY;
}

static int retire_step(const kb_principal_t *caller, int64_t team_id,
                       const db2_vault_rotation_row_t *row, const char *owner, int ttl)
{
   char actor[576];
   int64_t token = 0;
   if (claim(caller, team_id, row->id, "revoked", owner, ttl, &token, actor) != 0)
      return KB_VAULT_OP_RETRY;
   if (scope_begin(caller, team_id, actor) != 0)
      return KB_VAULT_OP_RETRY;
   int rc = db2_vault_rotation_transition_claimed(actor, row->id, owner, token, "revoked",
                                                  "retired", "");
   return scope_end(rc) == 0 ? KB_VAULT_OP_COMPLETE : KB_VAULT_OP_RETRY;
}

int kb_vault_rotation_ops_step(const kb_principal_t *caller, int64_t team_id, int64_t rotation_id,
                               const char *owner, int ttl_seconds)
{
   kb_vault_rotation_provider_t provider;
   void *provider_ctx = NULL;
   if (!caller || !owner || !*owner || ttl_seconds < 5 || ttl_seconds > 300 ||
       provider_snapshot(&provider, &provider_ctx) != 0 || !kb_vault_live_keys_allowed())
      return KB_VAULT_OP_DEFINITE_FAILURE;
   db2_vault_rotation_row_t row;
   char actor[576];
   if (load_row(caller, team_id, rotation_id, &row, actor) != 0)
      return KB_VAULT_OP_RETRY;
   if (row.compromise)
      return KB_VAULT_OP_DEFINITE_FAILURE;
   if (!strcmp(row.state, "provision"))
      return provision_step(caller, team_id, &row, owner, ttl_seconds, &provider, provider_ctx);
   if (!strcmp(row.state, "staged"))
      return probe_step(caller, team_id, &row, owner, ttl_seconds, &provider, provider_ctx);
   if (!strcmp(row.state, "probed") || !strcmp(row.state, "activating"))
   {
      (void)kb_vault_rotation_activate_or_resume(caller, team_id, rotation_id);
      return KB_VAULT_OP_RETRY;
   }
   if (!strcmp(row.state, "activated"))
      return revoke_step(caller, team_id, &row, owner, ttl_seconds, &provider, provider_ctx);
   if (!strcmp(row.state, "revoked"))
      return retire_step(caller, team_id, &row, owner, ttl_seconds);
   if (!strcmp(row.state, "retired"))
      return KB_VAULT_OP_COMPLETE;
   if (!strcmp(row.state, "failed"))
      return KB_VAULT_OP_REMEDIATION_REQUIRED;
   return KB_VAULT_OP_DEFINITE_FAILURE;
}

int kb_vault_rotation_ops_remediate(const kb_principal_t *caller, int64_t team_id,
                                    int64_t rotation_id, const char *owner, int ttl_seconds)
{
   kb_vault_rotation_provider_t provider;
   void *ctx = NULL;
   if (provider_snapshot(&provider, &ctx) != 0)
      return -1;
   db2_vault_rotation_row_t row;
   char actor[576], provision_op[128], revoke_op[128];
   char ref[DB2_VAULT_ROTATION_REF_MAX + 1] = "";
   char evidence[DB2_VAULT_ROTATION_REF_MAX + 1] = "";
   int64_t token = 0;
   if (load_row(caller, team_id, rotation_id, &row, actor) != 0 || row.compromise ||
       strcmp(row.state, "failed") ||
       claim(caller, team_id, rotation_id, "failed", owner, ttl_seconds, &token, actor) != 0 ||
       operation_key(rotation_id, "provision", provision_op) != 0 ||
       operation_key(rotation_id, "remediate-revoke-new", revoke_op) != 0)
      return -1;
   rotation_lease_ctx_t lease_ctx = {caller, team_id, rotation_id, owner, token, ttl_seconds};
   kb_vault_rotation_lease_t lease = lease_for(&lease_ctx);
   int rc;
   if (row.new_vendor_ref[0])
   {
      rc = provider.revoke(ctx, revoke_op, &row, &lease, row.new_vendor_ref, evidence,
                           sizeof(evidence));
   }
   else
   {
      int exists = 0;
      rc = provider.reconcile(ctx, provision_op, &row, &lease, ref, sizeof(ref), &exists, evidence,
                              sizeof(evidence));
      if (rc == KB_VAULT_OP_OK && exists)
      {
         if (!bounded_string(ref, sizeof(ref)))
            rc = KB_VAULT_OP_DEFINITE_FAILURE;
         else
         {
            evidence[0] = '\0';
            rc = provider.revoke(ctx, revoke_op, &row, &lease, ref, evidence, sizeof(evidence));
         }
      }
   }
   if (rc != KB_VAULT_OP_OK || !bounded_string(evidence, sizeof(evidence)))
   {
      release_best_effort(caller, team_id, rotation_id, owner, token);
      return -1;
   }
   unsigned char att[DB2_VAULT_ROTATION_ATTEST_MAX];
   size_t att_len = 0;
   uint64_t anchor = 0;
   rc = vault_hwm_read(row.key_id, &anchor, att, sizeof(att), &att_len);
   OPENSSL_cleanse(att, sizeof(att));
   if (rc != 0 || anchor != (uint64_t)row.from_version || anchor > INT64_MAX)
   {
      release_best_effort(caller, team_id, rotation_id, owner, token);
      return -1;
   }
   if (scope_begin(caller, team_id, actor) != 0)
      return -1;
   return scope_end(
       db2_vault_rotation_remediate(actor, rotation_id, owner, token, (int64_t)anchor, evidence));
}
