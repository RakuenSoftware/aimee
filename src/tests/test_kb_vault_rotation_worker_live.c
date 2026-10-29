/* test_kb_vault_rotation_worker_live.c: rotation requests through the daemon worker.
 *
 * Real Postgres and the signed-HWM KMS helper (scripts/run-vault-rotation-worker-live.sh
 * seeds team 982370, its egress binding and slot version 1). The vendor is a mock; the
 * request queue, the worker tick, the CLI and the fenced workflow are the real ones.
 * Covers: a filed request starts and drives a rotation to retired; a repeated pending
 * request is one request; an unknown model and a non-admin are refused; a failed
 * rotation waits for an explicit remediate request, which retires it. */
#include "kb/kb_vault_policy.h"
#include "kb/kb_vault_rotation.h"
#include "kb/kb_vault_rotation_cli.h"
#include "kb/kb_vault_rotation_ops.h"
#include "kb/kb_vault_rotation_worker.h"
#include "modules/vault/vault_store.h"
#include "modules/kb/c/db_postgres.h"
#include "modules/kb/c/kb_store.h"
#include "modules/kb/c/kb_store_internal.h"
#include "modules/kb/c/kb_store_tenant.h"
#include "modules/kb/c/org_vault_key_use.h"
#include "modules/kb/c/org_vault_rotation.h"
#include "modules/kb/c/vault_pg.h"
#include "modules/vault/vault_crypto.h"
#include "modules/vault/vault_internal.h"
#include "modules/vault/vault_kek_check.h"
#include "modules/vault/vault_server_key.h"
#include "modules/vault/vault_service.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEAM  982370
#define MODEL "rot-worker-model"

typedef struct
{
   int fail_probe;
   int provisions;
   int revokes;
} mock_vendor_t;

static mock_vendor_t g_vendor;

static void register_crypto_provider(void)
{
   const kb_store_vault_crypto_provider_t provider = {
       .aad_build_v2 = vault_aad_build_v2,
       .aad_build_v1_safe = vault_aad_build_v1_safe,
       .random = vault_crypto_random,
       .dek_wrap = vault_dek_wrap,
       .dek_unwrap = vault_dek_unwrap,
       .secret_encrypt = vault_secret_encrypt,
       .secret_decrypt = vault_secret_decrypt,
       .kek_check_wrap = vault_kek_check_wrap,
       .kek_check_verify = vault_kek_check_verify,
   };
   aimee_kb_store_register_vault_crypto_provider(&provider);
}

static kb_principal_t owner(void)
{
   kb_principal_t p = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   snprintf(p.subject, sizeof(p.subject), "owner");
   return p;
}

static int64_t scalar(const char *sql)
{
   kb_principal_t o = owner();
   assert(kb_store_tenant_scope_begin(&o, 0) == 0);
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(kb_store_conn(), sql, err, sizeof(err));
   assert(st && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   assert(kb_store_tenant_scope_commit() == 0);
   return value;
}

static int resolve_current(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                           const kb_vault_rotation_lease_t *lease, char *ref, size_t cap)
{
   (void)ctx;
   (void)op;
   (void)lease;
   snprintf(ref, cap, "old-%lld", (long long)row->from_version);
   return KB_VAULT_OP_OK;
}

static int provision(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                     const kb_vault_rotation_lease_t *lease, unsigned char *secret, size_t cap,
                     size_t *len, char *ref, size_t ref_cap, int *reconciled)
{
   (void)op;
   (void)lease;
   mock_vendor_t *v = ctx;
   int n = snprintf((char *)secret, cap,
                    "{\"access_key_id\":\"AKIAWORKER%lld\",\"secret_access_key\":\"s\"}",
                    (long long)row->to_version);
   assert(n > 0 && (size_t)n < cap);
   *len = (size_t)n;
   snprintf(ref, ref_cap, "new-%lld", (long long)row->to_version);
   *reconciled = 1;
   v->provisions++;
   return KB_VAULT_OP_OK;
}

static int probe(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                 const kb_vault_rotation_lease_t *lease, const unsigned char *secret, size_t len)
{
   (void)op;
   (void)row;
   (void)lease;
   (void)secret;
   (void)len;
   mock_vendor_t *v = ctx;
   return v->fail_probe ? KB_VAULT_OP_DEFINITE_FAILURE : KB_VAULT_OP_OK;
}

static int vendor_revoke(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                         const kb_vault_rotation_lease_t *lease, const char *ref, char *receipt,
                         size_t cap)
{
   (void)op;
   (void)row;
   (void)lease;
   mock_vendor_t *v = ctx;
   v->revokes++;
   snprintf(receipt, cap, "revoked:%s", ref);
   return KB_VAULT_OP_OK;
}

static int reconcile(void *ctx, const char *op, const kb_store_vault_rotation_row_t *row,
                     const kb_vault_rotation_lease_t *lease, char *ref, size_t ref_cap, int *exists,
                     char *evidence, size_t evidence_cap)
{
   (void)ctx;
   (void)op;
   (void)row;
   (void)lease;
   (void)ref;
   (void)ref_cap;
   *exists = 0;
   snprintf(evidence, evidence_cap, "no-orphan");
   return KB_VAULT_OP_OK;
}

/* The adoption check: STS GetCallerIdentity answers for any key it is shown. */
static int g_sts_calls;

static time_t fake_now(void)
{
   return time(NULL);
}

static int fake_sts(void *ctx, const char *authority, const char *const *names,
                    const char *const *values, size_t n, const char *body, size_t body_len,
                    int *status, char *response, size_t cap, size_t *response_len)
{
   (void)ctx;
   (void)names;
   (void)values;
   (void)n;
   (void)body_len;
   assert(!strcmp(authority, "sts.test") && strstr(body, "Action=GetCallerIdentity"));
   g_sts_calls++;
   *status = 200;
   int w = snprintf(response, cap,
                    "<GetCallerIdentityResponse><GetCallerIdentityResult>"
                    "<Arn>arn:aws:iam::123456789012:user/aimee-bedrock</Arn>"
                    "</GetCallerIdentityResult></GetCallerIdentityResponse>");
   assert(w > 0 && (size_t)w < cap);
   *response_len = (size_t)w;
   return 0;
}

static int file_request(const kb_principal_t *p, const char *kind, const char *model,
                        int64_t rotation, int64_t *id)
{
   char actor[576];
   assert(kb_identity_key(p, actor, sizeof(actor)) == 0);
   assert(kb_store_tenant_scope_begin(p, TEAM) == 0);
   int rc = kb_store_vault_rotation_request_add(actor, TEAM, kind, model, rotation, id);
   if (rc == 0)
      assert(kb_store_tenant_scope_commit() == 0);
   else
      kb_store_tenant_scope_rollback();
   return rc;
}

static kb_store_vault_rotation_request_row_t request_row(int64_t id)
{
   static kb_store_vault_rotation_request_row_t rows[50];
   kb_principal_t o = owner();
   assert(kb_store_tenant_scope_begin(&o, TEAM) == 0);
   int n = kb_store_vault_rotation_request_list("owner", TEAM, rows, 50);
   assert(n > 0 && kb_store_tenant_scope_commit() == 0);
   for (int i = 0; i < n; i++)
      if (rows[i].id == id)
         return rows[i];
   assert(!"request not listed");
   return rows[0];
}

static void ticks(int n)
{
   for (int i = 0; i < n; i++)
      kb_vault_rotation_worker_tick();
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !*url || !getenv("AIMEE_VAULT_KMS_HELPER") || !getenv("AIMEE_VAULT_KMS_KEY_ID"))
   {
      puts("SKIP: live PG + signed KMS HWM environment unavailable");
      return 0;
   }
   register_crypto_provider();
   /* The daemon registers this during module-stage setup; tenant scopes need it. */
   aimee_kb_store_register_identity_key_provider(kb_identity_key_from_fields);
   assert(kb_store_init(url) == 0);
   vault_store_set_backend(&vault_pg_backend);
   int64_t epoch = 0;
   int sealed = -1;
   assert(kb_store_vault_control_startup_begin(&epoch, &sealed) == 0 && epoch > 0);
   assert(kb_store_vault_control_startup_end(1) == 0);
   char err[256] = "";
   assert(kb_vault_policy_select("kms", err, sizeof(err)) == 0);
   assert(vault_primary_epoch_initialize((uint64_t)epoch) == VAULT_MAINTENANCE_OK);
   assert(kb_vault_live_keys_allowed());

   const kb_vault_rotation_provider_t provider = {resolve_current, provision, probe, vendor_revoke,
                                                  reconcile};
   assert(kb_vault_rotation_ops_register(&provider, &g_vendor) == 0);
   kb_vault_rotation_aws_t fake_aws = {
       .send = fake_sts, .now = fake_now, .iam_authority = "iam.test", .sts_authority = "sts.test"};
   kb_vault_rotation_worker_set_aws(&fake_aws);
   kb_principal_t o = owner();

   /* First provisioning, the production way: an ordinary vault write puts the
    * operator's key at version 1, which no rotation reached, so key use and
    * binding refuse it until the daemon adopts it. */
   {
      uint8_t kek[VAULT_KEK_LEN];
      assert(vault_server_kek(kek) == 0);
      assert(kb_store_tenant_scope_begin(&o, TEAM) == 0);
      assert(vault_store_set("team:982370:bedrock", kek, "bedrock", "iam",
                             "{\"access_key_id\":\"AKIAADOPTED1\","
                             "\"secret_access_key\":\"s\"}") == 0);
      assert(kb_store_tenant_scope_commit() == 0);
      memset(kek, 0, sizeof(kek));
   }
   char *bind_argv[] = {"aimee-kb",
                        "vault",
                        "bind",
                        "982370",
                        MODEL,
                        "--billable=rot-worker-billable",
                        "--pricing-version=1",
                        "--key-id=rot-worker-key",
                        "--principal=team:982370:bedrock",
                        "--agent=bedrock",
                        "--cred=iam",
                        "--max-input=1000",
                        "--max-output=100",
                        NULL};
   assert(kb_vault_rotation_cli(13, bind_argv, &o) == 1); /* nothing usable yet */
   char *adopt_argv[] = {"aimee-kb",
                         "vault",
                         "adopt",
                         "982370",
                         "--key-id=rot-worker-key",
                         "--principal=team:982370:bedrock",
                         "--agent=bedrock",
                         "--cred=iam",
                         NULL};
   assert(kb_vault_rotation_cli(8, adopt_argv, &o) == 0);
   ticks(2);
   assert(g_sts_calls == 1);
   assert(scalar("SELECT org_vault_has('team:982370:bedrock','bedrock','iam')") == 2);
   assert(scalar("SELECT count(*) FROM org_vault_rotation WHERE state='retired' AND "
                 "revoke_receipt='no-predecessor' AND to_version=2") == 1);
   assert(scalar("SELECT count(*) FROM org_rotation_request WHERE kind='adopt' AND "
                 "state='accepted'") == 1);
   assert(g_vendor.provisions == 0 && g_vendor.revokes == 0); /* nothing was vendored */
   assert(kb_vault_rotation_cli(8, adopt_argv, &o) == 1);     /* already usable */
   assert(kb_vault_rotation_cli(13, bind_argv, &o) == 0);

   /* Refusals: unknown model, and a team member who is not an admin. */
   int64_t id = 0;
   assert(file_request(&o, "rotate", "no-such-model", 0, &id) ==
          KB_STORE_VAULT_ROTATION_REQUEST_UNKNOWN);
   kb_principal_t member;
   assert(kb_principal_from_cert("issuer-rot-worker", "01AB", "worker-test", &member) == 0);
   char member_key[576], member_sql[800];
   assert(kb_identity_key(&member, member_key, sizeof(member_key)) == 0);
   snprintf(member_sql, sizeof(member_sql),
            "WITH i AS (INSERT INTO kb_team_membership(identity_key,team,is_default) "
            "VALUES ('%s',982370,1) ON CONFLICT DO NOTHING RETURNING 1) SELECT count(*) FROM i",
            member_key);
   (void)scalar(member_sql);
   assert(file_request(&member, "rotate", MODEL, 0, &id) == -1);

   /* The CLI files the request; a repeat while pending is the same request. */
   char *argv[] = {"aimee-kb", "vault", "rotate", "982370", MODEL, NULL};
   assert(kb_vault_rotation_cli(5, argv, &o) == 0);
   int64_t first = 0, again = 0;
   assert(file_request(&o, "rotate", MODEL, 0, &first) == 0);
   assert(file_request(&o, "rotate", MODEL, 0, &again) == 0 && again == first);
   assert(scalar("SELECT count(*) FROM org_rotation_request WHERE state='pending'") == 1);

   /* One tick takes the request and drives the rotation to retired. */
   ticks(3);
   kb_store_vault_rotation_request_row_t r = request_row(first);
   assert(!strcmp(r.state, "accepted") && r.rotation_id > 0);
   assert(!strcmp(r.rotation_state, "retired"));
   assert(g_vendor.provisions == 1 && g_vendor.revokes == 1);
   assert(scalar("SELECT org_vault_has('team:982370:bedrock','bedrock','iam')") == 3);
   char *list_argv[] = {"aimee-kb", "vault", "rotations", "982370", NULL};
   assert(kb_vault_rotation_cli(4, list_argv, &o) == 0);

   /* A failed rotation waits; only a remediate request retires it. */
   g_vendor.fail_probe = 1;
   int64_t second = 0;
   assert(file_request(&o, "rotate", MODEL, 0, &second) == 0 && second != first);
   ticks(3);
   r = request_row(second);
   assert(!strcmp(r.state, "accepted") && !strcmp(r.rotation_state, "failed"));
   int64_t failed_rotation = r.rotation_id;
   ticks(2);
   assert(!strcmp(request_row(second).rotation_state, "failed"));
   g_vendor.fail_probe = 0;
   int revokes_before = g_vendor.revokes;
   char rotation_text[32];
   snprintf(rotation_text, sizeof(rotation_text), "%lld", (long long)failed_rotation);
   char *fix_argv[] = {"aimee-kb", "vault", "remediate", "982370", rotation_text, NULL};
   assert(kb_vault_rotation_cli(5, fix_argv, &o) == 0);
   ticks(1);
   assert(g_vendor.revokes == revokes_before + 1); /* the staged, never-active key */
   assert(scalar("SELECT count(*) FROM org_vault_rotation WHERE state='retired'") == 3);
   assert(scalar("SELECT count(*) FROM org_rotation_request WHERE kind='remediate' AND "
                 "state='accepted'") == 1);
   assert(scalar("SELECT org_vault_has('team:982370:bedrock','bedrock','iam')") == 3);

   /* After remediation the slot rotates normally again. */
   int64_t third = 0;
   assert(file_request(&o, "rotate", MODEL, 0, &third) == 0);
   ticks(3);
   r = request_row(third);
   if (strcmp(r.rotation_state, "retired"))
      fprintf(stderr, "third rotation: request %s (%s), rotation %s\n", r.state, r.detail,
              r.rotation_state);
   assert(!strcmp(r.rotation_state, "retired"));
   assert(scalar("SELECT org_vault_has('team:982370:bedrock','bedrock','iam')") == 4);
   /* Egress still accepts the slot: its current version's rotation is retired,
    * not merely activated, and the remediated rotation vouches for nothing. */
   assert(scalar("SELECT org_egress_binding_set(982370,'rot-worker-model',"
                 "'rot-worker-billable',1,'rot-worker-key','team:982370:bedrock','bedrock',"
                 "'iam',1000,100,true)::int") == 1);
   assert(scalar("SELECT count(*) FROM org_vault_rotation WHERE state='retired' AND "
                 "failure_phase<>'' AND to_version=4") == 1);
   assert(scalar("SELECT count(*) FROM kb_audit_outbox WHERE action LIKE 'vault.rotation.%' AND "
                 "detail ILIKE '%secret_access_key%'") == 0);

   /* A schedule files the rotation once the last one is older than its interval;
    * age the slot's completed rotations by two days instead of waiting a day. */
   char *sched_argv[] = {"aimee-kb", "vault", "schedule", "982370", MODEL, "1", NULL};
   assert(kb_vault_rotation_cli(6, sched_argv, &o) == 0);
   kb_vault_rotation_worker_schedule_now();
   ticks(1);
   assert(scalar("SELECT org_vault_has('team:982370:bedrock','bedrock','iam')") == 4);
   assert(scalar("WITH u AS (UPDATE org_vault_rotation SET updated_at=to_char((now() AT TIME "
                 "ZONE 'UTC') - interval '2 days','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                 "RETURNING 1) SELECT count(*) FROM u") >= 1);
   kb_vault_rotation_worker_schedule_now();
   ticks(2);
   assert(scalar("SELECT org_vault_has('team:982370:bedrock','bedrock','iam')") == 5);
   kb_vault_rotation_worker_schedule_now();
   ticks(1); /* just rotated: not due again */
   assert(scalar("SELECT org_vault_has('team:982370:bedrock','bedrock','iam')") == 5);
   char *off_argv[] = {"aimee-kb", "vault", "schedule", "982370", MODEL, "off", NULL};
   assert(kb_vault_rotation_cli(6, off_argv, &o) == 0);
   assert(scalar("SELECT count(*) FROM org_rotation_schedule") == 0);

   assert(kb_vault_policy_select("file", err, sizeof(err)) == 0);
   kb_store_shutdown();
   puts("PASS: adopt -> bind -> rotate -> remediate -> scheduled rotation, through the worker");
   return 0;
}
