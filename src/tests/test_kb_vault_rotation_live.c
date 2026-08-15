#include "kb/kb_vault_rotation.h"
#include "modules/vault/vault_custody_kms.h"
#include "modules/vault/vault_internal.h"
#include "modules/vault/vault_server_key.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static kb_principal_t owner(void)
{
   kb_principal_t p = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   return p;
}

static int64_t scalar(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return value;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   const char *key_id = getenv("AIMEE_VAULT_KMS_KEY_ID");
   if (!url || !url[0] || !key_id || !key_id[0] || !getenv("AIMEE_VAULT_KMS_HELPER"))
   {
      puts("SKIP: live PG + signed KMS HWM environment unavailable");
      return 0;
   }
   assert(db2_init(url) == 0);
   kb_principal_t caller = owner();

   /* Seed N=1 inside a short tenant transaction. */
   assert(db2_tenant_scope_begin(&caller, 0) == 0);
   assert(scalar("SELECT org_vault_put('org:test:p7-live',NULL,'bedrock','primary',1,"
                 "'\\x0102','\\x0304','\\x0506','\\x0708')") == 1);
   assert(db2_tenant_scope_commit() == 0);

   vault_custody_set_provider(vault_custody_kms_provider());
   int64_t rid = 0;
   assert(kb_vault_rotation_start(&caller, 0, key_id, "org:test:p7-live", "bedrock", "primary", 1,
                                  0, &rid) == 0);
   const uint8_t envelope[] = {0x11, 0x22, 0x33};
   assert(kb_vault_rotation_stage(&caller, 0, rid, envelope, sizeof(envelope), envelope,
                                  sizeof(envelope), envelope, sizeof(envelope), envelope,
                                  sizeof(envelope)) == 0);
   assert(kb_vault_rotation_mark_probed(&caller, 0, rid) == 0);

   assert(db2_tenant_scope_begin(&caller, 0) == 0);
   assert(scalar("SELECT org_vault_has('org:test:p7-live','bedrock','primary')") == 1);
   assert(db2_tenant_scope_commit() == 0);

   /* Inject the exact crash window: advance the verified external anchor, then
    * "restart" before DB finalization. Resume must observe N+1 and finalize only. */
   uint8_t att[64];
   size_t att_len = 0;
   uint64_t anchor = 0;
   assert(vault_hwm_read(key_id, &anchor, att, sizeof(att), &att_len) == 0 && anchor == 1);
   assert(vault_hwm_cas(key_id, 1, 2, att, sizeof(att), &att_len) == 0);
   assert(kb_vault_rotation_activate_or_resume(&caller, 0, rid) == KB_VAULT_ROTATION_COMPLETE);

   assert(db2_tenant_scope_begin(&caller, 0) == 0);
   assert(scalar("SELECT org_vault_has('org:test:p7-live','bedrock','primary')") == 2);
   assert(scalar("SELECT count(*) FROM org_vault_rotation WHERE id=(SELECT max(id) FROM "
                 "org_vault_rotation) AND state='activated' AND hwm_attestation IS NOT NULL") == 1);
   assert(db2_tenant_scope_commit() == 0);

   vault_custody_set_provider(NULL);
   db2_shutdown();
   puts("PASS: live PG17 + signed KMS HWM crash recovery");
   return 0;
}
