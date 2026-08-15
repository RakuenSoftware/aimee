#include "kb/kb_vault_key_use.h"
#include "kb/kb_vault_protected_use.h"

#include "modules/db2/c/org_vault_key_use.h"
#include "modules/vault/vault_crypto.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int g_hwm_fail;
static int g_verify_fail;
static int g_candidate_fail;
static int g_admit_result = 1;
static int64_t g_admit_epoch = 1;
static int g_begin_fail;
static uint64_t g_seen_admitted_epoch;
static int g_sealed;
static int g_callback_fail;
static int g_callback_calls;
static int g_scope_open;
static int g_live = 1;
static int g_decrypt_legacy;
static const unsigned char g_secret[] = "bedrock-secret";

int kb_identity_key(const kb_principal_t *p, char *out, size_t cap)
{
   if (!p || !p->authenticated || cap < 6)
      return -1;
   snprintf(out, cap, "id:%d", (int)p->kind);
   return 0;
}

int kb_vault_live_keys_allowed(void)
{
   return g_live;
}

int db2_tenant_scope_begin(const kb_principal_t *p, int64_t team)
{
   if (!p || !p->authenticated || team < 1 || g_scope_open)
      return -1;
   g_scope_open = 1;
   return 0;
}

int db2_tenant_scope_commit(void)
{
   assert(g_scope_open);
   g_scope_open = 0;
   return 0;
}

void db2_tenant_scope_rollback(void)
{
   g_scope_open = 0;
}

int vault_hwm_read(const char *key_id, uint64_t *version, uint8_t *att, size_t cap, size_t *att_len)
{
   if (g_hwm_fail || !key_id || cap < 3)
      return -1;
   *version = 2;
   memcpy(att, "att", 3);
   *att_len = 3;
   return 0;
}

int vault_hwm_verify(const char *key_id, uint64_t version, const uint8_t *att, size_t att_len)
{
   return g_verify_fail || !key_id || version != 2 || att_len != 3 || memcmp(att, "att", 3) != 0
              ? -1
              : 0;
}

int db2_vault_key_use_candidate(const char *actor, int64_t team, const char *key_id,
                                const char *principal, const char *agent, const char *cred,
                                int64_t version, db2_vault_key_use_envelope_t *out)
{
   if (g_candidate_fail || !actor || team != 7 || !key_id || !principal || !agent || !cred ||
       version != 2)
      return g_candidate_fail == 2 ? -2 : -1;
   memset(out, 0, sizeof(*out));
   out->version = 2;
   out->ciphertext_len = sizeof(g_secret) - 1;
   memcpy(out->hwm_attestation, "att", 3);
   out->hwm_attestation_len = 3;
   return 0;
}

int db2_vault_key_use_admit(const char *actor, int64_t team, const char *origin, const char *use_id,
                            const char *key_id, const char *principal, const char *agent,
                            const char *cred, int64_t version, const char *digest,
                            const char *provider, const char *model, const char *operation,
                            const uint8_t *att, size_t att_len, db2_vault_key_use_envelope_t *out)
{
   if (!actor || team != 7 || !origin || !use_id || !key_id || !principal || !agent || !cred ||
       version != 2 || !digest || !provider || !model || !operation || att_len != 3)
      return -1;
   if (g_admit_result < 0)
      return g_admit_result;
   memset(out, 0, sizeof(*out));
   out->seal_epoch = g_admit_epoch;
   if (g_admit_result == 0)
      return 0;
   out->version = 2;
   out->ciphertext_len = sizeof(g_secret) - 1;
   memcpy(out->hwm_attestation, "att", 3);
   out->hwm_attestation_len = 3;
   return 1;
}

uint64_t vault_use_epoch_snapshot(void)
{
   return 9;
}

int vault_use_begin(uint64_t epoch, uint64_t admitted_epoch, uint8_t kek[VAULT_KEK_LEN])
{
   g_seen_admitted_epoch = admitted_epoch;
   if (g_begin_fail || epoch != 9 || admitted_epoch != (uint64_t)g_admit_epoch)
      return -1;
   memset(kek, 0x55, VAULT_KEK_LEN);
   return 0;
}

void vault_use_end(void)
{
}

int vault_is_sealed(void)
{
   return g_sealed;
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

int vault_dek_unwrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN],
                     uint8_t dek[VAULT_DEK_LEN])
{
   (void)wrapped;
   if (!kek || kek[0] != 0x55)
      return -1;
   memset(dek, 0x66, VAULT_DEK_LEN);
   return 0;
}

int vault_secret_decrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                         const uint8_t nonce[VAULT_GCM_NONCE_LEN], const uint8_t *ciphertext,
                         size_t ciphertext_len, const uint8_t tag[VAULT_GCM_TAG_LEN],
                         uint8_t *plaintext)
{
   (void)nonce;
   (void)ciphertext;
   (void)tag;
   static const char v2[] = "v2:vault|bedrock|primary|2";
   static const char v1[] = "vault|bedrock|primary|2";
   static const char custom[] = "token-root-aad";
   int aad_ok = g_decrypt_legacy
                    ? aad_len == sizeof(v1) - 1 && !memcmp(aad, v1, sizeof(v1) - 1)
                    : ((aad_len == sizeof(v2) - 1 && !memcmp(aad, v2, sizeof(v2) - 1)) ||
                       (aad_len == sizeof(custom) - 1 && !memcmp(aad, custom, sizeof(custom) - 1)));
   if (!dek || dek[0] != 0x66 || !aad || !aad_ok || ciphertext_len != sizeof(g_secret) - 1)
      return -1;
   memcpy(plaintext, g_secret, sizeof(g_secret) - 1);
   return 0;
}

static int consume(const unsigned char *secret, size_t len, void *ctx)
{
   (void)ctx;
   g_callback_calls++;
   assert(len == sizeof(g_secret) - 1 && memcmp(secret, g_secret, len) == 0);
   return g_callback_fail ? -1 : 0;
}

static kb_vault_key_use_status_t run(void)
{
   kb_principal_t actor = {.kind = KB_PRIN_OIDC, .authenticated = 1};
   kb_principal_t origin = {.kind = KB_PRIN_CERT, .authenticated = 1};
   return kb_vault_key_use(&actor, 7, &origin, "use-1", "key-1", "vault", "bedrock", "primary",
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                           "bedrock", "claude", "invoke", consume, NULL);
}

static void reset(void)
{
   g_hwm_fail = g_verify_fail = g_candidate_fail = g_begin_fail = g_sealed = 0;
   g_callback_fail = g_callback_calls = g_scope_open = 0;
   g_seen_admitted_epoch = 0;
   g_admit_result = 1;
   g_admit_epoch = 1;
   g_live = 1;
   g_decrypt_legacy = 0;
}

static void test_prebuilt_aad(void)
{
   static const uint8_t correct[] = "token-root-aad";
   static const uint8_t wrong[] = "token-root-bad";
   db2_vault_key_use_envelope_t e;
   memset(&e, 0, sizeof(e));
   e.seal_epoch = 23;
   e.version = 2;
   e.ciphertext_len = sizeof(g_secret) - 1;
   reset();
   g_admit_epoch = 23;
   assert(kb_vault_protected_use_with_aad(9, &e, correct, sizeof(correct) - 1, consume, NULL) ==
          KB_VAULT_KEY_USE_OK);
   assert(g_callback_calls == 1 && g_seen_admitted_epoch == 23);
   reset();
   g_admit_epoch = 23;
   assert(kb_vault_protected_use_with_aad(9, &e, wrong, sizeof(wrong) - 1, consume, NULL) ==
          KB_VAULT_KEY_USE_INTEGRITY);
   assert(g_callback_calls == 0);
   assert(kb_vault_protected_use_with_aad(9, &e, NULL, 0, consume, NULL) ==
          KB_VAULT_KEY_USE_INTEGRITY);
   assert(g_callback_calls == 0);
}

int main(void)
{
   reset();
   g_admit_epoch = 17;
   assert(run() == KB_VAULT_KEY_USE_OK && g_callback_calls == 1 && g_seen_admitted_epoch == 17);
   reset();
   g_admit_result = 0;
   assert(run() == KB_VAULT_KEY_USE_REPLAY && g_callback_calls == 0);
   reset();
   g_hwm_fail = 1;
   assert(run() == KB_VAULT_KEY_USE_RETRY && g_callback_calls == 0);
   reset();
   g_verify_fail = 1;
   assert(run() == KB_VAULT_KEY_USE_UNATTESTED && g_callback_calls == 0);
   reset();
   g_candidate_fail = 1;
   assert(run() == KB_VAULT_KEY_USE_RETRY && g_callback_calls == 0);
   reset();
   g_candidate_fail = 2;
   assert(run() == KB_VAULT_KEY_USE_UNATTESTED && g_callback_calls == 0);
   reset();
   g_live = 0;
   assert(run() == KB_VAULT_KEY_USE_SEALED && g_callback_calls == 0);
   reset();
   g_admit_result = -1;
   assert(run() == KB_VAULT_KEY_USE_RETRY && g_callback_calls == 0);
   reset();
   g_admit_result = DB2_VAULT_KEY_USE_INTEGRITY;
   assert(run() == KB_VAULT_KEY_USE_INTEGRITY && g_callback_calls == 0);
   reset();
   g_admit_result = DB2_VAULT_KEY_USE_SEALED;
   assert(run() == KB_VAULT_KEY_USE_SEALED && g_callback_calls == 0);
   reset();
   g_admit_epoch = 0;
   assert(run() == KB_VAULT_KEY_USE_INTEGRITY && g_callback_calls == 0);
   reset();
   g_admit_result = 0;
   g_admit_epoch = 0;
   assert(run() == KB_VAULT_KEY_USE_INTEGRITY && g_callback_calls == 0);
   reset();
   g_begin_fail = g_sealed = 1;
   assert(run() == KB_VAULT_KEY_USE_SEALED && g_callback_calls == 0);
   reset();
   g_begin_fail = 1;
   assert(run() == KB_VAULT_KEY_USE_RETRY && g_callback_calls == 0);
   reset();
   g_callback_fail = 1;
   assert(run() == KB_VAULT_KEY_USE_CALLBACK_FAILED && g_callback_calls == 1);
   reset();
   g_decrypt_legacy = 1;
   assert(run() == KB_VAULT_KEY_USE_OK && g_callback_calls == 1);
   test_prebuilt_aad();
   int old_state = -1;
   assert(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state) == 0);
   assert(old_state == PTHREAD_CANCEL_ENABLE);
   assert(pthread_setcancelstate(old_state, NULL) == 0);
   puts("kb_vault_key_use: all tests passed");
   return 0;
}
