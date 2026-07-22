#include "kb/kb_vault_rotation_ops.h"
#include "kb/kb_vault_rotation.h"
#include "modules/vault/vault_crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static db2_vault_rotation_row_t g_row;
static db2_vault_rotation_envelope_t g_envelope;
static int g_scope;
static int g_get_fail;
static int g_provision_calls, g_probe_calls, g_revoke_calls;
static int g_resolve_result;
static int g_revoke_result;
static int g_unterminated_ref;
static int g_probe_admit_fail;
static int g_fail_checkpoint_fail;
static int g_reconciled = 1;
static int g_different_secret;

int kb_identity_key(const kb_principal_t *p, char *out, size_t cap)
{
   if (!p || !p->authenticated || cap < 6)
      return -1;
   snprintf(out, cap, "owner");
   return 0;
}
int db2_tenant_scope_begin(const kb_principal_t *p, int64_t team)
{
   assert(p && p->authenticated && team == 7 && !g_scope);
   return g_scope = 1, 0;
}
int db2_tenant_scope_commit(void)
{
   assert(g_scope);
   return g_scope = 0, 0;
}
void db2_tenant_scope_rollback(void)
{
   assert(g_scope);
   g_scope = 0;
}
int kb_vault_live_keys_allowed(void)
{
   return 1;
}
int db2_vault_rotation_get(int64_t id, db2_vault_rotation_row_t *out)
{
   assert(g_scope && id == g_row.id);
   if (g_get_fail)
      return -1;
   *out = g_row;
   return 0;
}
int db2_vault_rotation_claim(const char *actor, int64_t id, const char *expected, const char *owner,
                             int ttl, int64_t *token)
{
   assert(g_scope && actor && id == g_row.id && !strcmp(expected, g_row.state) && owner &&
          ttl >= 5);
   g_row.claim_token++;
   snprintf(g_row.claim_owner, sizeof(g_row.claim_owner), "%s", owner);
   *token = g_row.claim_token;
   return 0;
}
int db2_vault_rotation_release(const char *actor, int64_t id, const char *owner, int64_t token)
{
   assert(g_scope && actor && id == g_row.id && !strcmp(owner, g_row.claim_owner) &&
          token == g_row.claim_token);
   g_row.claim_owner[0] = 0;
   return 0;
}
int db2_vault_rotation_heartbeat(const char *actor, int64_t id, const char *owner, int64_t token,
                                 int ttl)
{
   assert(g_scope && actor && id == g_row.id && owner && token == g_row.claim_token && ttl >= 5);
   return 0;
}
int db2_vault_rotation_checkpoint_old_ref(const char *actor, int64_t id, const char *owner,
                                          int64_t token, const char *ref)
{
   assert(g_scope && actor && id == g_row.id && owner && token == g_row.claim_token && ref);
   snprintf(g_row.old_vendor_ref, sizeof(g_row.old_vendor_ref), "%s", ref);
   return 0;
}
int db2_vault_rotation_stage_claimed(const char *actor, int64_t id, const char *owner,
                                     int64_t token, const char *ref,
                                     const db2_vault_rotation_envelope_t *e)
{
   assert(g_scope && actor && id == g_row.id && owner && token == g_row.claim_token && ref && e);
   g_envelope = *e;
   snprintf(g_row.new_vendor_ref, sizeof(g_row.new_vendor_ref), "%s", ref);
   snprintf(g_row.state, sizeof(g_row.state), "staged");
   return 0;
}
int db2_vault_rotation_probe_admit(const char *actor, int64_t id, const char *owner, int64_t token,
                                   const char *op, db2_vault_rotation_envelope_t *e)
{
   assert(g_scope && actor && id == g_row.id && owner && token == g_row.claim_token && op);
   if (g_probe_admit_fail)
      return -1;
   *e = g_envelope;
   return 0;
}
int db2_vault_rotation_transition_claimed(const char *actor, int64_t id, const char *owner,
                                          int64_t token, const char *expected, const char *next,
                                          const char *receipt)
{
   assert(g_scope && actor && id == g_row.id && owner && token == g_row.claim_token &&
          !strcmp(expected, g_row.state));
   snprintf(g_row.state, sizeof(g_row.state), "%s", next);
   if (receipt)
      snprintf(g_row.revoke_receipt, sizeof(g_row.revoke_receipt), "%s", receipt);
   return 0;
}
int db2_vault_rotation_fail_claimed(const char *actor, int64_t id, const char *owner, int64_t token,
                                    const char *expected, const char *phase, const char *error)
{
   (void)error;
   assert(g_scope && actor && id == g_row.id && owner && token == g_row.claim_token &&
          !strcmp(expected, g_row.state));
   if (g_fail_checkpoint_fail)
      return -1;
   snprintf(g_row.state, sizeof(g_row.state), "failed");
   snprintf(g_row.failure_phase, sizeof(g_row.failure_phase), "%s", phase);
   return 0;
}
int db2_vault_rotation_remediate(const char *actor, int64_t id, const char *owner, int64_t token,
                                 int64_t anchor, const char *evidence)
{
   assert(g_scope && actor && id == g_row.id && owner && token == g_row.claim_token &&
          anchor == g_row.from_version && evidence && *evidence);
   snprintf(g_row.state, sizeof(g_row.state), "retired");
   return 0;
}
int kb_vault_rotation_activate_or_resume(const kb_principal_t *p, int64_t team, int64_t id)
{
   assert(!g_scope && p && team == 7 && id == g_row.id);
   snprintf(g_row.state, sizeof(g_row.state), "activated");
   return KB_VAULT_ROTATION_COMPLETE;
}
int vault_server_kek(unsigned char out[VAULT_KEK_LEN])
{
   assert(!g_scope);
   memset(out, 0x44, VAULT_KEK_LEN);
   return 0;
}
int vault_crypto_random(uint8_t *out, size_t n)
{
   memset(out, 0x55, n);
   return 0;
}
int vault_aad_build_v2(const char *principal, const char *agent, const char *cred, int64_t version,
                       uint8_t *out, size_t cap, size_t *out_len)
{
   int n =
       snprintf((char *)out, cap, "v2:%s|%s|%s|%lld", principal, agent, cred, (long long)version);
   if (n < 0 || (size_t)n >= cap)
      return -1;
   *out_len = (size_t)n;
   return 0;
}
int vault_aad_build_v1_safe(const char *principal, const char *agent, const char *cred,
                            int64_t version, uint8_t *out, size_t cap, size_t *out_len)
{
   int n = snprintf((char *)out, cap, "%s|%s|%s|%lld", principal, agent, cred, (long long)version);
   if (n < 0 || (size_t)n >= cap)
      return -1;
   *out_len = (size_t)n;
   return 0;
}
int vault_secret_encrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t an,
                         const uint8_t *pt, size_t pn, uint8_t nonce[VAULT_GCM_NONCE_LEN],
                         uint8_t *ct, uint8_t tag[VAULT_GCM_TAG_LEN])
{
   assert(dek && aad && an && pt && pn);
   memset(nonce, 0x66, VAULT_GCM_NONCE_LEN);
   memcpy(ct, pt, pn);
   memset(tag, 0x77, VAULT_GCM_TAG_LEN);
   return 0;
}
int vault_secret_decrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t an,
                         const uint8_t nonce[VAULT_GCM_NONCE_LEN], const uint8_t *ct, size_t cn,
                         const uint8_t tag[VAULT_GCM_TAG_LEN], uint8_t *pt)
{
   assert(dek && aad && an && nonce && tag);
   memcpy(pt, ct, cn);
   return 0;
}
int vault_dek_wrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t dek[VAULT_DEK_LEN],
                   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   assert(kek && dek);
   memset(wrapped, 0x88, VAULT_WRAPPED_DEK_LEN);
   return 0;
}
int vault_dek_unwrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN],
                     uint8_t dek[VAULT_DEK_LEN])
{
   assert(kek && wrapped);
   memset(dek, 0x55, VAULT_DEK_LEN);
   return 0;
}
int vault_hwm_read(const char *key, uint64_t *version, uint8_t *att, size_t cap, size_t *len)
{
   assert(!g_scope && key && cap >= 4);
   *version = (uint64_t)g_row.from_version;
   memset(att, 1, 4);
   *len = 4;
   return 0;
}

static int resolve(void *ctx, const char *op, const db2_vault_rotation_row_t *row,
                   const kb_vault_rotation_lease_t *lease, char *ref, size_t cap)
{
   (void)ctx;
   assert(!g_scope && op && row && lease && lease->heartbeat);
   if (g_resolve_result != KB_VAULT_OP_OK)
      return g_resolve_result;
   if (g_unterminated_ref)
   {
      memset(ref, 'x', cap);
      return KB_VAULT_OP_OK;
   }
   snprintf(ref, cap, "old-ref");
   return KB_VAULT_OP_OK;
}
static int provision(void *ctx, const char *op, const db2_vault_rotation_row_t *row,
                     const kb_vault_rotation_lease_t *lease, unsigned char *secret, size_t cap,
                     size_t *len, char *ref, size_t ref_cap, int *reconciled)
{
   (void)ctx;
   assert(!g_scope && op && row && lease && cap >= 7);
   assert(lease->heartbeat(lease->heartbeat_ctx) == 0);
   g_provision_calls++;
   memcpy(secret, g_different_secret ? "newone" : "sekret", 6);
   *len = 6;
   snprintf(ref, ref_cap, "new-ref");
   *reconciled = g_reconciled;
   return KB_VAULT_OP_OK;
}
static int probe(void *ctx, const char *op, const db2_vault_rotation_row_t *row,
                 const kb_vault_rotation_lease_t *lease, const unsigned char *secret, size_t len)
{
   (void)ctx;
   assert(!g_scope && op && row && lease && len == 6 && !memcmp(secret, "sekret", 6));
   g_probe_calls++;
   return KB_VAULT_OP_OK;
}
static int revoke(void *ctx, const char *op, const db2_vault_rotation_row_t *row,
                  const kb_vault_rotation_lease_t *lease, const char *ref, char *receipt,
                  size_t cap)
{
   (void)ctx;
   assert(!g_scope && op && row && lease && ref && *ref);
   g_revoke_calls++;
   if (g_revoke_result != KB_VAULT_OP_OK)
      return g_revoke_result;
   snprintf(receipt, cap, "confirmed-unusable");
   return KB_VAULT_OP_OK;
}
static int reconcile(void *ctx, const char *op, const db2_vault_rotation_row_t *row,
                     const kb_vault_rotation_lease_t *lease, char *ref, size_t ref_cap, int *exists,
                     char *evidence, size_t evidence_cap)
{
   (void)ctx;
   assert(!g_scope && op && row && lease && !strcmp(op, "aimee:p7:rotation:42:provision"));
   *exists = 0;
   ref[0] = 0;
   (void)ref_cap;
   snprintf(evidence, evidence_cap, "confirmed-absent");
   return KB_VAULT_OP_OK;
}

int main(void)
{
   const kb_principal_t caller = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   const kb_vault_rotation_provider_t provider = {resolve, provision, probe, revoke, reconcile};
   assert(kb_vault_rotation_ops_register(&provider, NULL) == 0);
   assert(kb_vault_rotation_ops_register(&provider, NULL) == -1);
   memset(&g_row, 0, sizeof(g_row));
   g_row.id = 42;
   g_row.team_id = 7;
   g_row.has_team = 1;
   g_row.from_version = 1;
   g_row.to_version = 2;
   snprintf(g_row.key_id, sizeof(g_row.key_id), "team:7|bedrock|primary");
   snprintf(g_row.principal, sizeof(g_row.principal), "team:7");
   snprintf(g_row.agent, sizeof(g_row.agent), "bedrock");
   snprintf(g_row.cred, sizeof(g_row.cred), "primary");
   snprintf(g_row.state, sizeof(g_row.state), "provision");

   g_get_fail = 1;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   g_get_fail = 0;

   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   assert(!strcmp(g_row.state, "staged") && g_provision_calls == 1);
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   assert(!strcmp(g_row.state, "probed") && g_probe_calls == 1);
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   assert(!strcmp(g_row.state, "activated"));
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   assert(!strcmp(g_row.state, "revoked") && g_revoke_calls == 1);
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_COMPLETE);
   assert(!strcmp(g_row.state, "retired"));

   snprintf(g_row.state, sizeof(g_row.state), "activated");
   g_revoke_result = KB_VAULT_OP_DEFINITE_FAILURE;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   assert(!strcmp(g_row.state, "activated"));
   g_revoke_result = KB_VAULT_OP_OK;

   snprintf(g_row.state, sizeof(g_row.state), "staged");
   g_probe_admit_fail = 1;
   int probes_before = g_probe_calls;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   assert(g_probe_calls == probes_before);
   g_probe_admit_fail = 0;

   snprintf(g_row.state, sizeof(g_row.state), "failed");
   snprintf(g_row.failure_phase, sizeof(g_row.failure_phase), "provision");
   g_row.new_vendor_ref[0] = 0;
   assert(kb_vault_rotation_ops_remediate(&caller, 7, 42, "worker-b", 30) == 0);
   assert(!strcmp(g_row.state, "retired"));

   snprintf(g_row.state, sizeof(g_row.state), "provision");
   g_row.compromise = 1;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) ==
          KB_VAULT_OP_DEFINITE_FAILURE);
   snprintf(g_row.state, sizeof(g_row.state), "failed");
   assert(kb_vault_rotation_ops_remediate(&caller, 7, 42, "worker-a", 30) == -1);

   g_row.compromise = 0;
   snprintf(g_row.state, sizeof(g_row.state), "provision");
   g_resolve_result = KB_VAULT_OP_DEFINITE_FAILURE;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) ==
          KB_VAULT_OP_DEFINITE_FAILURE);
   assert(!strcmp(g_row.state, "failed") && !strcmp(g_row.failure_phase, "provision"));

   snprintf(g_row.state, sizeof(g_row.state), "provision");
   g_fail_checkpoint_fail = 1;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) == KB_VAULT_OP_RETRY);
   assert(!strcmp(g_row.state, "provision"));
   g_fail_checkpoint_fail = 0;

   snprintf(g_row.state, sizeof(g_row.state), "provision");
   g_resolve_result = KB_VAULT_OP_OK;
   g_unterminated_ref = 1;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) ==
          KB_VAULT_OP_DEFINITE_FAILURE);
   assert(!strcmp(g_row.state, "failed"));

   snprintf(g_row.state, sizeof(g_row.state), "provision");
   g_unterminated_ref = 0;
   g_reconciled = 0;
   g_different_secret = 1;
   assert(kb_vault_rotation_ops_step(&caller, 7, 42, "worker-a", 30) ==
          KB_VAULT_OP_DEFINITE_FAILURE);
   assert(!strcmp(g_row.state, "failed"));
   puts("PASS: fenced vendor rotation operations");
   return 0;
}
