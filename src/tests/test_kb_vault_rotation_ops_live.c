#include "kb/kb_vault_policy.h"
#include "kb/kb_vault_rotation.h"
#include "kb/kb_vault_rotation_ops.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   char provision_op[128];
   char probe_op[128];
   char revoke_op[128];
   int provision_calls;
   int probe_calls;
   int revoke_calls;
   int candidate_count;
   int old_revoked;
} mock_vendor_t;

static kb_principal_t owner(void)
{
   kb_principal_t p = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   return p;
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

static void stable_op(char saved[128], const char *got)
{
   assert(got && *got);
   if (!saved[0])
      snprintf(saved, 128, "%s", got);
   else
      assert(!strcmp(saved, got));
}

static int resolve_current(void *opaque, const char *op, const db2_vault_rotation_row_t *row,
                           const kb_vault_rotation_lease_t *lease, char *ref, size_t cap)
{
   mock_vendor_t *v = opaque;
   stable_op(v->provision_op, op);
   assert(row && lease && !strcmp(row->state, "provision"));
   snprintf(ref, cap, "vendor-old-credential");
   return KB_VAULT_OP_OK;
}

static int provision(void *opaque, const char *op, const db2_vault_rotation_row_t *row,
                     const kb_vault_rotation_lease_t *lease, unsigned char *secret, size_t cap,
                     size_t *len, char *ref, size_t ref_cap, int *reconciled)
{
   static const unsigned char value[] = "vendor-secret-live";
   mock_vendor_t *v = opaque;
   stable_op(v->provision_op, op);
   assert(row && lease && cap >= sizeof(value) - 1);
   assert(lease->heartbeat(lease->heartbeat_ctx) == 0);
   if (v->candidate_count == 0)
      v->candidate_count = 1;
   memcpy(secret, value, sizeof(value) - 1);
   *len = sizeof(value) - 1;
   snprintf(ref, ref_cap, "vendor-new-credential");
   /* The operation key was reconciled before every attempt, including the first. */
   *reconciled = 1;
   v->provision_calls++;
   /* Model a lost one-time response after the vendor created the credential. */
   return v->provision_calls == 1 ? KB_VAULT_OP_UNCERTAIN : KB_VAULT_OP_OK;
}

static int probe(void *opaque, const char *op, const db2_vault_rotation_row_t *row,
                 const kb_vault_rotation_lease_t *lease, const unsigned char *secret, size_t len)
{
   static const unsigned char value[] = "vendor-secret-live";
   mock_vendor_t *v = opaque;
   stable_op(v->probe_op, op);
   assert(row && lease && len == sizeof(value) - 1 && !memcmp(secret, value, len));
   v->probe_calls++;
   return v->probe_calls == 1 ? KB_VAULT_OP_UNCERTAIN : KB_VAULT_OP_OK;
}

static int vendor_revoke(void *opaque, const char *op, const db2_vault_rotation_row_t *row,
                         const kb_vault_rotation_lease_t *lease, const char *ref, char *receipt,
                         size_t cap)
{
   mock_vendor_t *v = opaque;
   stable_op(v->revoke_op, op);
   assert(row && lease && !strcmp(ref, "vendor-old-credential"));
   v->old_revoked = 1;
   v->revoke_calls++;
   /* Model a lost revoke response; the retry queries the same operation/result. */
   if (v->revoke_calls == 1)
      return KB_VAULT_OP_UNCERTAIN;
   snprintf(receipt, cap, "vendor-confirms-unusable");
   return KB_VAULT_OP_OK;
}

static int reconcile(void *opaque, const char *op, const db2_vault_rotation_row_t *row,
                     const kb_vault_rotation_lease_t *lease, char *ref, size_t ref_cap, int *exists,
                     char *evidence, size_t evidence_cap)
{
   (void)opaque;
   (void)op;
   (void)row;
   (void)lease;
   (void)ref;
   (void)ref_cap;
   (void)exists;
   (void)evidence;
   (void)evidence_cap;
   return KB_VAULT_OP_DEFINITE_FAILURE;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !*url || !getenv("AIMEE_VAULT_KMS_HELPER") || !getenv("AIMEE_VAULT_KMS_KEY_ID"))
   {
      puts("SKIP: live PG + signed KMS HWM environment unavailable");
      return 0;
   }
   assert(db2_init(url) == 0);
   char err[256] = "";
   assert(kb_vault_policy_select("kms", err, sizeof(err)) == 0);
   kb_principal_t caller = owner();

   assert(db2_tenant_scope_begin(&caller, 0) == 0);
   assert(scalar("SELECT org_vault_put('org:test:p7-ops-live',NULL,'bedrock','primary',1,"
                 "decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),'\\x03',"
                 "decode(repeat('04',16),'hex'))") == 1);
   assert(db2_tenant_scope_commit() == 0);

   int64_t rid = 0;
   const char *key_id = getenv("AIMEE_VAULT_KMS_KEY_ID");
   assert(kb_vault_rotation_start(&caller, 0, key_id, "org:test:p7-ops-live", "bedrock", "primary",
                                  1, 0, &rid) == 0);
   mock_vendor_t vendor = {0};
   const kb_vault_rotation_provider_t provider = {resolve_current, provision, probe, vendor_revoke,
                                                  reconcile};
   assert(kb_vault_rotation_ops_register(&provider, &vendor) == 0);

   int rc = KB_VAULT_OP_RETRY;
   for (int i = 0; i < 16 && rc != KB_VAULT_OP_COMPLETE; ++i)
      rc = kb_vault_rotation_ops_step(&caller, 0, rid, "ct260-worker", 30);
   assert(rc == KB_VAULT_OP_COMPLETE);
   assert(vendor.provision_calls == 2 && vendor.candidate_count == 1);
   assert(vendor.probe_calls == 2 && vendor.revoke_calls == 2 && vendor.old_revoked);

   assert(db2_tenant_scope_begin(&caller, 0) == 0);
   assert(scalar("SELECT org_vault_has('org:test:p7-ops-live','bedrock','primary')") == 2);
   assert(scalar("SELECT count(*) FROM org_vault_rotation WHERE state='retired' AND "
                 "old_vendor_ref='vendor-old-credential' AND "
                 "new_vendor_ref='vendor-new-credential' AND "
                 "revoke_receipt='vendor-confirms-unusable'") == 1);
   assert(scalar("SELECT count(*) FROM org_vault_secret WHERE "
                 "principal='org:test:p7-ops-live' AND "
                 "ciphertext=convert_to('vendor-secret-live','UTF8')") == 0);
   assert(scalar("SELECT count(*) FROM kb_audit_event WHERE action LIKE 'vault.rotation.%' AND "
                 "detail ILIKE '%vendor-secret-live%'") == 0);
   assert(db2_tenant_scope_commit() == 0);

   assert(kb_vault_policy_select("file", err, sizeof(err)) == 0);
   db2_shutdown();
   puts("PASS: live fenced rotation + crash reconciliation + signed KMS HWM");
   return 0;
}
