#include "kb/http/kb_http_egress.h"
#include "kb/kb_vault_policy.h"
#include "kb/kb_vault_rotation.h"
#include "kb_enroll.h"
#include "kb_pki.h"
#include "kb_tls.h"
#include "kb_curator_drain.h"
#include "kb_curator_llm.h"
#include "kb_curator_queue.h"
#include "kb_curator_serve.h"
#include "runtime_secret.h"
#include "modules/kb-synthesis/kb_curator_notify.h"
#include "modules/vault/vault_crypto.h"
#include "modules/vault/vault_internal.h"
#include "modules/vault/vault_server_key.h"
#include "modules/vault/vault_service.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/enrollments.h"
#include "modules/db2/c/org_vault_key_use.h"
#include "modules/db2/c/vault_pg.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This live fixture links the full KB route closure so it can exercise the
 * real mTLS listener and egress route. Curator workers are unrelated to that
 * path and are not started here; provide their link-only boundary with inert
 * behavior instead of pulling background threads and provider dispatch into a
 * credential-transport test. */
void kb_curator_queue_counts(kb_curator_queue_counts_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
}

void kb_curator_invalidation_broadcast(const char *project, const char *file_path,
                                       int artifacts_stale)
{
   (void)project;
   (void)file_path;
   (void)artifacts_stale;
}

int kb_curator_implements_json(const char *topic, char *out, size_t out_cap)
{
   (void)topic;
   if (out && out_cap)
      out[0] = '\0';
   return -1;
}

int kb_curator_synthesize_serve_json(const char *topic, char *out, size_t out_cap)
{
   return kb_curator_implements_json(topic, out, out_cap);
}

int kb_curator_contradictions_json(int limit, char *out, size_t out_cap)
{
   (void)limit;
   return kb_curator_implements_json(NULL, out, out_cap);
}

int kb_curator_queue_docs_for_project(const char *project)
{
   (void)project;
   return 0;
}

int kb_curator_code_unit_jobs_delete_project(const char *project)
{
   (void)project;
   return 0;
}

struct cJSON *kb_curator_stages_json(void)
{
   return NULL;
}

struct cJSON *kb_curator_presets_json(void)
{
   return NULL;
}

char *kb_curator_llm_run(kb_curator_stage_t stage, const char *system_prompt,
                         const char *request_json, struct cJSON *json_schema,
                         const char *fallback_command, int out_cap, char *errbuf, size_t errlen)
{
   (void)stage;
   (void)system_prompt;
   (void)request_json;
   (void)json_schema;
   (void)fallback_command;
   (void)out_cap;
   if (errbuf && errlen)
      errbuf[0] = '\0';
   return NULL;
}

static int64_t scalar(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st || aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      fprintf(stderr, "p2b live SQL failed: %s\n", err);
      if (st)
         aimee_pg_finalize(st);
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
       db2_conn(), "INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES (?1,?2,1)",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identity);
   aimee_pg_bind_int64(st, "?2", team);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_DONE ? 0 : -1;
}

static int revoke_enrollment(const char *fingerprint)
{
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "UPDATE kb_enrollments SET state='revoked',revoked_at=pg_now_text() "
                        "WHERE fingerprint=?1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", fingerprint);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return step == AIMEE_PG_DONE && changed == 1 ? 0 : -1;
}

static kb_principal_t owner(void)
{
   kb_principal_t p = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   return p;
}

static int test_pam_check_credentials(const char *user, const char *password)
{
   return user && password && strcmp(user, "p2b-live") == 0 &&
          strcmp(password, "p2b-live-password") == 0;
}

static int egress_request(int port, const char *ca, const char *cert, const char *key,
                          const char *request_id, int64_t team, char *response, size_t response_cap)
{
   char body[2048];
   int n = snprintf(body, sizeof(body),
                    "{\"request_id\":\"%s\",\"team_id\":%lld,"
                    "\"model_id\":\"p2b-live-model\",\"stream\":false,"
                    "\"payload\":{\"model\":\"p2b-live-model\",\"messages\":[{"
                    "\"role\":\"user\",\"content\":\"reply with ok\"}],"
                    "\"max_tokens\":16,\"stream\":false}}",
                    request_id, (long long)team);
   assert(n > 0 && (size_t)n < sizeof(body));
   int status = 0;
   assert(kb_tls_client_request_auth("localhost", port, ca, cert, key, "POST", "/v1/llm/egress",
                                     body,
                                     "Authorization: Bearer p2b-live-token\r\n"
                                     "X-Aimee-Service-Authorization: Basic "
                                     "cDJiLWxpdmU6cDJiLWxpdmUtcGFzc3dvcmQ=\r\n",
                                     response, response_cap, &status) == 0);
   return status;
}

int main(void)
{
   if (getenv("AIMEE_P2B_EXPECT_DISABLED"))
   {
      kb_principal_t transport = {.kind = KB_PRIN_CERT, .authenticated = 1};
      char response[256] = "";
      int status =
          kb_http_egress_route("POST", "/v1/llm/egress", "{}", 2, &transport,
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
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN",
                               "scope:service:p2b-live:p2b-live-token") == 0);
   kb_tls_set_pam_check_for_test(test_pam_check_credentials);
   assert(kb_mtls_start(0, aimee_home, "localhost") == 0);
   int mtls_port = kb_mtls_bound_port();
   assert(mtls_port > 0);
   char conn[1024], ca[KB_PKI_CERT_PEM_MAX], cert[KB_PKI_CERT_PEM_MAX];
   char client_key[KB_PKI_KEY_PEM_MAX];
   assert(kb_enroll_mint(aimee_home, "localhost", mtls_port, "service:p2b-live", conn,
                         sizeof(conn)) == 0);
   assert(kb_tls_enroll(conn, ca, sizeof(ca), cert, sizeof(cert), client_key, sizeof(client_key)) ==
          0);
   char cert_fp[KB_PKI_FP_HEX], cert_issuer[256], cert_serial[128], identity[512];
   assert(kb_pki_ca_fingerprint(cert, cert_fp, sizeof(cert_fp)) == 0);
   assert(kb_pki_cert_metadata(cert, cert_issuer, sizeof(cert_issuer), cert_serial,
                               sizeof(cert_serial)) == 0);
   kb_principal_t probe;
   assert(kb_principal_from_cert(cert_issuer, cert_serial, "service:p2b-live", &probe) == 0);
   assert(kb_identity_key(&probe, identity, sizeof(identity)) == 0);
   assert(add_member(identity, team) == 0);
   char authority[33];
   int authority_rc =
       db2_enrollment_authority_resolve(cert_fp, cert_issuer, probe.subject, authority);
   if (authority_rc != 0)
      fprintf(stderr, "P2b enrolled identity unresolved fp=%s issuer=%s serial=%s rc=%d\n", cert_fp,
              cert_issuer, cert_serial, authority_rc);
   assert(authority_rc == 0);
   int scope_rc = db2_tenant_scope_begin(&probe, team);
   if (scope_rc != 0)
      fprintf(stderr, "P2b enrolled membership unresolved identity=%s rc=%d\n", identity, scope_rc);
   assert(scope_rc == 0 && db2_tenant_scope_commit() == 0);
   char response[262144];
   int status =
       egress_request(mtls_port, ca, cert, client_key, "11111111-1111-4111-8111-111111111111", team,
                      response, sizeof(response));
   if (status != 200)
      fprintf(stderr,
              "P2b route status=%d response=%s reserved=%lld in_flight=%lld failed=%lld "
              "key_uses=%lld\n",
              status, response,
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

   /* Exact replay is durable and never re-dispatches. */
   status = egress_request(mtls_port, ca, cert, client_key, "11111111-1111-4111-8111-111111111111",
                           team, response, sizeof(response));
   assert(status == 409 && strstr(response, "request already recorded"));

   /* Both private refusal classes collapse to 429 and stop before P7/network. */
   assert(db2_tenant_scope_begin(&admin, team) == 0);
   assert(scalar("SELECT org_rate_policy_set('team','982260',3600,0)") > 0);
   assert(db2_tenant_scope_commit() == 0);
   status = egress_request(mtls_port, ca, cert, client_key, "22222222-2222-4222-8222-222222222222",
                           team, response, sizeof(response));
   assert(status == 429);
   assert(db2_tenant_scope_begin(&admin, team) == 0);
   assert(scalar("SELECT org_rate_policy_set('team','982260',3600,100)") > 0);
   assert(scalar("SELECT org_budget_set(982260,NULL,'day',(SELECT spend_usd+reserved_usd "
                 "FROM org_budget_counter WHERE team_id=982260 AND project_id IS NULL "
                 "AND period='day'),NULL)") > 0);
   assert(db2_tenant_scope_commit() == 0);
   status = egress_request(mtls_port, ca, cert, client_key, "33333333-3333-4333-8333-333333333333",
                           team, response, sizeof(response));
   assert(status == 429);
   assert(db2_tenant_scope_begin(&admin, team) == 0);
   assert(scalar("SELECT org_budget_set(982260,NULL,'day',1000,NULL)") > 0);
   assert(db2_tenant_scope_commit() == 0);

   status = egress_request(mtls_port, ca, cert, client_key, "44444444-4444-4444-8444-444444444444",
                           team + 1, response, sizeof(response));
   assert(status == 403);

   /* Authenticated complete provider denial is 502/durable denied and charges
    * the reservation; retry remains a no-send 409. */
   status = egress_request(mtls_port, ca, cert, client_key, "55555555-5555-4555-8555-555555555555",
                           team, response, sizeof(response));
   assert(status == 502);
   assert(scalar("SELECT count(*) FROM org_egress_dispatch WHERE request_id="
                 "'55555555-5555-4555-8555-555555555555' AND state='denied' AND "
                 "http_status=429 AND realized_usd=reserved_max_usd") == 1);
   status = egress_request(mtls_port, ca, cert, client_key, "55555555-5555-4555-8555-555555555555",
                           team, response, sizeof(response));
   assert(status == 409);

   /* Partial authenticated response is ambiguous, charges the reservation,
    * returns 504, and cannot be dispatched again. */
   status = egress_request(mtls_port, ca, cert, client_key, "66666666-6666-4666-8666-666666666666",
                           team, response, sizeof(response));
   assert(status == 504);
   assert(scalar("SELECT count(*) FROM org_egress_dispatch WHERE request_id="
                 "'66666666-6666-4666-8666-666666666666' AND state='uncertain' AND "
                 "http_status=0 AND realized_usd=reserved_max_usd") == 1);
   status = egress_request(mtls_port, ca, cert, client_key, "66666666-6666-4666-8666-666666666666",
                           team, response, sizeof(response));
   assert(status == 409);

   assert(scalar("SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=982260") == 3);

   assert(db2_tenant_scope_begin(&admin, team) == 0);
   assert(scalar("SELECT org_egress_binding_set(982260,'p2b-live-model',"
                 "'p2b-live-billable',1,'p2b-live-key','team:982260:bedrock',"
                 "'bedrock','iam',1000,100,false)::int") == 1);
   assert(db2_tenant_scope_commit() == 0);
   status = egress_request(mtls_port, ca, cert, client_key, "77777777-7777-4777-8777-777777777777",
                           team, response, sizeof(response));
   assert(status == 403);

   assert(revoke_enrollment(cert_fp) == 0);
   status = egress_request(mtls_port, ca, cert, client_key, "88888888-8888-4888-8888-888888888888",
                           team, response, sizeof(response));
   assert(status == 401);

   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(dek, sizeof(dek));
   OPENSSL_cleanse(wrapped, sizeof(wrapped));
   OPENSSL_cleanse(ciphertext, sizeof(ciphertext));
   OPENSSL_cleanse(client_key, sizeof(client_key));
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   kb_mtls_stop();
   kb_tls_set_pam_check_for_test(NULL);
   db2_shutdown();
   puts("PASS: enrolled mTLS -> admission -> vault-sign -> TLS dispatch -> IR settlement");
   return 0;
}
