#include "kb/kb_vault_key_use.h"
#include "kb/kb_vault_policy.h"
#include "kb/kb_vault_rotation.h"
#include "modules/vault/vault_custody_kms.h"
#include "modules/vault/vault_crypto.h"
#include "modules/vault/vault_internal.h"
#include "modules/vault/vault_server_key.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/org_vault_key_use.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t secret[] = "AKIA_TEST\nmock-secret-access-key";
static int callback_calls;

static kb_principal_t owner(void)
{
   kb_principal_t p = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   return p;
}

static kb_principal_t origin(void)
{
   kb_principal_t p = {.kind = KB_PRIN_CERT, .authenticated = 1};
   snprintf(p.issuer, sizeof(p.issuer), "CN=p7-test-ca");
   snprintf(p.subject, sizeof(p.subject), "01");
   return p;
}

static int consume(const unsigned char *plaintext, size_t len, void *ctx)
{
   (void)ctx;
   callback_calls++;
   return len == sizeof(secret) - 1 && CRYPTO_memcmp(plaintext, secret, len) == 0 ? 0 : -1;
}

static int64_t scalar(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return value;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   const char *key_id = getenv("AIMEE_VAULT_KMS_KEY_ID");
   const char *team_text = getenv("AIMEE_TEST_TEAM_ID");
   if (!url || !*url || !key_id || !*key_id || !team_text || !*team_text ||
       !getenv("AIMEE_VAULT_KMS_HELPER"))
   {
      puts("SKIP: live PG + signed KMS key-use environment unavailable");
      return 0;
   }
   int64_t team_id = strtoll(team_text, NULL, 10);
   assert(team_id > 0 && db2_init(url) == 0);
   int64_t startup_epoch = 0;
   int startup_sealed = -1;
   assert(db2_vault_control_startup_begin(&startup_epoch, &startup_sealed) == 0);
   assert(startup_epoch > 0 && (startup_sealed == 0 || startup_sealed == 1));
   assert(db2_vault_control_startup_end(1) == 0);
   kb_principal_t caller = owner();
   kb_principal_t transport = origin();
   char policy_err[256] = "";
   assert(kb_vault_policy_select("kms", policy_err, sizeof(policy_err)) == 0);

   uint8_t kek[VAULT_KEK_LEN], dek[VAULT_DEK_LEN], wrapped[VAULT_WRAPPED_DEK_LEN];
   uint8_t nonce[VAULT_GCM_NONCE_LEN], ciphertext[sizeof(secret) - 1], tag[VAULT_GCM_TAG_LEN];
   assert(vault_server_kek(kek) == 0 && vault_crypto_random(dek, sizeof(dek)) == 0 &&
          vault_dek_wrap(kek, dek, wrapped) == 0);
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   assert(vault_aad_build_v2("team:970713:provider:bedrock", "bedrock", "primary", 2, aad,
                             sizeof(aad), &aad_len) == 0);
   assert(vault_secret_encrypt(dek, aad, aad_len, secret, sizeof(secret) - 1, nonce, ciphertext,
                               tag) == 0);

   assert(db2_tenant_scope_begin(&caller, team_id) == 0);
   assert(scalar("SELECT org_vault_put('team:970713:provider:bedrock',970713,'bedrock',"
                 "'primary',1,decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),"
                 "decode('03','hex'),decode(repeat('04',16),'hex'))") == 1);
   assert(db2_tenant_scope_commit() == 0);

   int64_t rid = 0;
   assert(kb_vault_rotation_start(&caller, team_id, key_id, "team:970713:provider:bedrock",
                                  "bedrock", "primary", 1, 0, &rid) == 0);
   assert(kb_vault_rotation_stage(&caller, team_id, rid, wrapped, sizeof(wrapped), nonce,
                                  sizeof(nonce), ciphertext, sizeof(ciphertext), tag,
                                  sizeof(tag)) == 0);
   assert(kb_vault_rotation_mark_probed(&caller, team_id, rid) == 0);
   assert(kb_vault_rotation_activate_or_resume(&caller, team_id, rid) ==
          KB_VAULT_ROTATION_COMPLETE);

   const char digest[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
   assert(kb_vault_key_use(&caller, team_id, &transport, "live-use-1", key_id,
                           "team:970713:provider:bedrock", "bedrock", "primary", digest, "bedrock",
                           "anthropic.claude", "invoke", consume, NULL) == KB_VAULT_KEY_USE_OK);
   assert(callback_calls == 1);
   assert(kb_vault_key_use(&caller, team_id, &transport, "live-use-1", key_id,
                           "team:970713:provider:bedrock", "bedrock", "primary", digest, "bedrock",
                           "anthropic.claude", "invoke", consume, NULL) == KB_VAULT_KEY_USE_REPLAY);
   assert(callback_calls == 1);

   assert(db2_tenant_scope_begin(&caller, team_id) == 0);
   assert(scalar("SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=970713") == 1);
   assert(scalar("SELECT count(*) FROM kb_audit_event WHERE action='vault.key_use' AND "
                 "subject='team:970713|bedrock|primary'") == 1);
   assert(db2_tenant_scope_commit() == 0);

   assert(db2_tenant_scope_begin(&caller, team_id) == 0);
   assert(scalar("WITH u AS (UPDATE org_vault_current SET version=1 WHERE "
                 "principal='team:970713:provider:bedrock' AND agent='bedrock' AND "
                 "cred='primary' RETURNING 1) SELECT count(*) FROM u") == 1);
   assert(db2_tenant_scope_commit() == 0);
   assert(kb_vault_key_use(&caller, team_id, &transport, "live-use-rollback", key_id,
                           "team:970713:provider:bedrock", "bedrock", "primary", digest, "bedrock",
                           "anthropic.claude", "invoke", consume,
                           NULL) == KB_VAULT_KEY_USE_UNATTESTED);
   assert(callback_calls == 1);
   assert(db2_tenant_scope_begin(&caller, team_id) == 0);
   assert(scalar("WITH u AS (UPDATE org_vault_current SET version=2 WHERE "
                 "principal='team:970713:provider:bedrock' AND agent='bedrock' AND "
                 "cred='primary' RETURNING 1) SELECT count(*) FROM u") == 1);
   assert(scalar("WITH u AS (UPDATE org_vault_secret SET hwm_attestation='\\xdead' WHERE "
                 "principal='team:970713:provider:bedrock' AND agent='bedrock' AND "
                 "cred='primary' AND version=2 RETURNING 1) SELECT count(*) FROM u") == 1);
   assert(db2_tenant_scope_commit() == 0);
   assert(kb_vault_key_use(&caller, team_id, &transport, "live-use-bad-signature", key_id,
                           "team:970713:provider:bedrock", "bedrock", "primary", digest, "bedrock",
                           "anthropic.claude", "invoke", consume,
                           NULL) == KB_VAULT_KEY_USE_UNATTESTED);
   assert(callback_calls == 1);
   assert(db2_tenant_scope_begin(&caller, team_id) == 0);
   assert(scalar("SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=970713") == 1);
   assert(scalar("SELECT count(*) FROM kb_audit_event WHERE detail LIKE '%AKIA_TEST%'") == 0);
   assert(db2_tenant_scope_commit() == 0);

   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(dek, sizeof(dek));
   OPENSSL_cleanse(wrapped, sizeof(wrapped));
   OPENSSL_cleanse(ciphertext, sizeof(ciphertext));
   assert(kb_vault_policy_select("file", policy_err, sizeof(policy_err)) == 0);
   db2_shutdown();
   puts("PASS: live PG17 + signed HWM + locked decrypt/callback/replay");
   return 0;
}
