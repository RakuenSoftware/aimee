#include "kb/http/kb_http_egress.h"
#include "kb/kb_vault_policy.h"
#include "kb/kb_vault_rotation.h"
#include "kb_enroll.h"
#include "kb_pki.h"
#include "kb_tls.h"
#include "modules/vault/vault_crypto.h"
#include "modules/vault/vault_internal.h"
#include "modules/vault/vault_server_key.h"
#include "modules/vault/vault_service.h"
#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/db2_tenant.h"
#include "db2/enrollments.h"
#include "db2/org_vault_key_use.h"
#include "db2/vault_pg.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t scalar(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st || aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      fprintf(stderr, "p2b live SQL failed: %s\n", err);
      if (st) aimee_pg_finalize(st);
      return -1;
   }
   int64_t value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static int add_member(const char *identity, int64_t team)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES (?1,?2,1)",
       err, sizeof(err));
   if (!st) return -1;
   aimee_pg_bind_text(st, "?1", identity);
   aimee_pg_bind_int64(st, "?2", team);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_DONE ? 0 : -1;
}

static kb_principal_t owner(void)
{
   kb_principal_t p = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   return p;
}

int main(void)
{
   if (getenv("AIMEE_P2B_EXPECT_DISABLED"))
   {
      kb_principal_t transport = {.kind = KB_PRIN_CERT, .authenticated = 1};
      char response[256] = "";
      int status = kb_http_egress_route(
          "POST", "/v1/llm/egress", "{}", 2, &transport,
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          response, sizeof(response));
      assert(status == 503 && strstr(response, "egress unavailable"));
      puts("PASS: hardened P2b artifact denies before parsing, DB, vault, or network");
      return 0;
   }
   const char *url = getenv("AIMEE_TEST_PG_URL");
   const char *key_id = getenv("AIMEE_VAULT_KMS_KEY_ID");
   if (!url || !*url || !key_id || !*key_id || !getenv("AIMEE_VAULT_KMS_HELPER"))
   {
      puts("SKIP: P2b live PG/KMS environment unavailable");
      return 0;
   }
   const int64_t team = 982260;
   const char *principal = "team:982260:bedrock";
   const unsigned char plaintext[] =
       "{\"access_key_id\":\"AKIDEXAMPLE\",\"secret_access_key\":\"secret\"}";

   assert(db2_init(url) == 0);
   vault_store_set_backend(&vault_pg_backend);
   int64_t epoch = 0;
   int sealed = -1;
   assert(db2_vault_control_startup_begin(&epoch, &sealed) == 0 && epoch > 0);
   assert(db2_vault_control_startup_end(1) == 0);
   char policy_err[256] = "";
   assert(kb_vault_policy_select("kms", policy_err, sizeof(policy_err)) == 0);
   assert(vault_primary_epoch_initialize((uint64_t)epoch) == VAULT_MAINTENANCE_OK);
   assert(kb_egress_release_allowed());

   kb_principal_t admin = owner();
   uint8_t kek[VAULT_KEK_LEN], dek[VAULT_DEK_LEN], wrapped[VAULT_WRAPPED_DEK_LEN];
   uint8_t nonce[VAULT_GCM_NONCE_LEN], ciphertext[sizeof(plaintext) - 1], tag[VAULT_GCM_TAG_LEN];
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   assert(vault_server_kek(kek) == 0 && vault_crypto_random(dek, sizeof(dek)) == 0);
   assert(vault_dek_wrap(kek, dek, wrapped) == 0);
   assert(vault_aad_build_v2(principal, "bedrock", "iam", 2, aad, sizeof(aad), &aad_len) == 0);
   assert(vault_secret_encrypt(dek, aad, aad_len, plaintext, sizeof(plaintext) - 1, nonce,
                               ciphertext, tag) == 0);

   int64_t rotation = 0;
   assert(kb_vault_rotation_start(&admin, team, key_id, principal, "bedrock", "iam", 1, 0,
                                  &rotation) == 0);
   assert(kb_vault_rotation_stage(&admin, team, rotation, wrapped, sizeof(wrapped), nonce,
                                  sizeof(nonce), ciphertext, sizeof(ciphertext), tag,
                                  sizeof(tag)) == 0);
   assert(kb_vault_rotation_mark_probed(&admin, team, rotation) == 0);
   assert(kb_vault_rotation_activate_or_resume(&admin, team, rotation) ==
          KB_VAULT_ROTATION_COMPLETE);

   assert(db2_tenant_scope_begin(&admin, team) == 0);
   assert(scalar("SELECT org_egress_binding_set(982260,'p2b-live-model','p2b-live-billable',1,"
                 "'p2b-live-key','team:982260:bedrock','bedrock','iam',1000,100,true)::int") == 1);
   assert(db2_tenant_scope_commit() == 0);

   const char *aimee_home = getenv("AIMEE_HOME");
   assert(aimee_home && *aimee_home);
   assert(kb_mtls_start(0, aimee_home, "localhost") == 0);
   int mtls_port = kb_mtls_bound_port();
   assert(mtls_port > 0);
   char conn[1024], ca[KB_PKI_CERT_PEM_MAX], cert[KB_PKI_CERT_PEM_MAX];
   char client_key[KB_PKI_KEY_PEM_MAX];
   assert(kb_enroll_mint(aimee_home, "localhost", mtls_port, "p2b-live", conn,
                         sizeof(conn)) == 0);
   assert(kb_tls_enroll(conn, ca, sizeof(ca), cert, sizeof(cert), client_key,
                        sizeof(client_key)) == 0);
   char cert_fp[KB_PKI_FP_HEX], cert_issuer[256], cert_serial[128], identity[512];
   assert(kb_pki_ca_fingerprint(cert, cert_fp, sizeof(cert_fp)) == 0);
   assert(kb_pki_cert_metadata(cert, cert_issuer, sizeof(cert_issuer), cert_serial,
                               sizeof(cert_serial)) == 0);
   kb_principal_t probe;
   assert(kb_principal_from_cert(cert_issuer, cert_serial, "p2b-live", &probe) == 0);
   assert(kb_identity_key(&probe, identity, sizeof(identity)) == 0);
   assert(add_member(identity, team) == 0);
   char authority[33];
   int authority_rc = db2_enrollment_authority_resolve(cert_fp, cert_issuer, cert_serial,
                                                        authority);
   if (authority_rc != 0)
      fprintf(stderr, "P2b enrolled identity unresolved fp=%s issuer=%s serial=%s rc=%d\n",
              cert_fp, cert_issuer, cert_serial, authority_rc);
   assert(authority_rc == 0);
   int scope_rc = db2_tenant_scope_begin(&probe, team);
   if (scope_rc != 0)
      fprintf(stderr, "P2b enrolled membership unresolved identity=%s rc=%d\n", identity,
              scope_rc);
   assert(scope_rc == 0 && db2_tenant_scope_commit() == 0);
   const char *body =
       "{\"request_id\":\"11111111-1111-4111-8111-111111111111\","
       "\"team_id\":982260,\"model_id\":\"p2b-live-model\",\"stream\":false,"
       "\"payload\":{\"model\":\"p2b-live-model\",\"messages\":[{\"role\":\"user\","
       "\"content\":\"reply with ok\"}],\"max_tokens\":16,\"stream\":false}}";
   char response[262144];
   int status = 0;
   assert(kb_tls_client_request("localhost", mtls_port, ca, cert, client_key, "POST",
                                "/v1/llm/egress", body, response, sizeof(response),
                                &status) == 0);
   if (status != 200)
      fprintf(stderr, "P2b route status=%d response=%s reserved=%lld in_flight=%lld failed=%lld "
                      "key_uses=%lld\n", status, response,
              (long long)scalar("SELECT count(*) FROM org_egress_dispatch WHERE state='reserved'"),
              (long long)scalar("SELECT count(*) FROM org_egress_dispatch WHERE state='in_flight'"),
              (long long)scalar("SELECT count(*) FROM org_egress_dispatch WHERE state='failed'"),
              (long long)scalar("SELECT count(*) FROM org_vault_key_use_intent"));
   assert(status == 200);
   assert(strstr(response, "\"request_id\":\"11111111-1111-4111-8111-111111111111\"") &&
          strstr(response, "\"team_id\":982260") &&
          strstr(response, "\"content\":\"mock-completion\""));
   assert(scalar("SELECT count(*) FROM org_egress_dispatch WHERE team_id=982260 AND "
                 "state='succeeded' AND prompt_tokens>=0 AND completion_tokens>0") == 1);
   assert(scalar("SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=982260") == 1);

   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(dek, sizeof(dek));
   OPENSSL_cleanse(wrapped, sizeof(wrapped));
   OPENSSL_cleanse(ciphertext, sizeof(ciphertext));
   OPENSSL_cleanse(client_key, sizeof(client_key));
   kb_mtls_stop();
   db2_shutdown();
   puts("PASS: enrolled mTLS -> admission -> vault-sign -> TLS dispatch -> IR settlement");
   return 0;
}
