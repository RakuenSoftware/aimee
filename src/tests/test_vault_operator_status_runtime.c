#include "modules/db2/c/vault_operator_status_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_tx, g_closed, g_reads, g_motion, g_null_mismatch, g_database_integrity;
static db2_vault_operator_snapshot_t g_value;
static const char *states[] = {
    "",         "preparing", "custody_prepared", "wraps_staged", "reseal_committing",
    "resealed", "promoted",  "completed",        "aborted",      "recovery_required"};

static void *open_db(void *ctx, const char *dsn, int64_t deadline, char *err, size_t len)
{
   (void)ctx;
   (void)deadline;
   (void)err;
   (void)len;
   return dsn && *dsn ? &g_tx : NULL;
}
static void close_db(void *ctx, void *db)
{
   (void)ctx;
   assert(db == &g_tx);
   ++g_closed;
}
static void put_i64(char out[129], int64_t value)
{
   snprintf(out, 129, "%lld", (long long)value);
}
static int query_db(void *ctx, void *db, const char *sql, int64_t deadline,
                    db2_vault_operator_db_result_t *r, char *err, size_t len)
{
   (void)ctx;
   (void)deadline;
   (void)err;
   (void)len;
   assert(db == &g_tx);
   memset(r, 0, sizeof(*r));
   if (!strcmp(sql, "BEGIN"))
   {
      assert(!g_tx);
      g_tx = 1;
      return 0;
   }
   if (!strcmp(sql, "COMMIT") || !strcmp(sql, "ROLLBACK"))
   {
      g_tx = 0;
      return 0;
   }
   if (!strncmp(sql, "SET ", 4))
      return 0;
   r->rows = 1;
   if (strstr(sql, "FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()"))
   {
      if (g_database_integrity)
         return DB2_VAULT_OPERATOR_INTEGRITY;
      int64_t motion = (g_motion && (g_reads++ & 1)) ? 1 : 0;
      r->columns = 11;
      put_i64(r->value[0][0], g_value.seal_epoch + motion);
      strcpy(r->value[0][1], g_value.sealed ? "t" : "f");
      put_i64(r->value[0][2], g_value.control_fence);
      put_i64(r->value[0][3], g_value.last_opened_fence);
      if (!g_value.operation_present)
         for (int i = 4; i < 11; ++i)
            r->is_null[0][i] = 1;
      else
      {
         strcpy(r->value[0][4], "000102030405060708090a0b0c0d0e0f");
         strcpy(r->value[0][5], states[g_value.operation_state]);
         put_i64(r->value[0][6], g_value.operation_seal_epoch);
         put_i64(r->value[0][7], g_value.operation_fence);
         put_i64(r->value[0][8], g_value.old_generation);
         put_i64(r->value[0][9], g_value.new_generation);
         strcpy(r->value[0][10], g_value.failure_class);
         if (g_null_mismatch)
            r->is_null[0][5] = 1;
      }
   }
   else
   {
      r->columns = 1;
      strcpy(r->value[0][0], "t");
   }
   return 0;
}
static int idle_db(void *ctx, void *db)
{
   (void)ctx;
   assert(db == &g_tx);
   return !g_tx;
}
static const db2_vault_operator_db_vtable_t ops = {open_db, close_db, query_db, idle_db};
static int provider(void *ctx, db2_vault_provider_status_t *out)
{
   *out = *(db2_vault_provider_status_t *)ctx;
   return 0;
}
static void base(int sealed)
{
   memset(&g_value, 0, sizeof(g_value));
   g_value.seal_epoch = 7;
   g_value.control_fence = 9;
   g_value.sealed = sealed;
   g_reads = g_motion = g_null_mismatch = g_database_integrity = 0;
}

int main(void)
{
   db2_vault_operator_runtime_t runtime;
   db2_vault_operator_status_t status;
   char error[128] = "";
   int tcp = -1;
   assert(db2_vault_operator_conninfo_allowed_for_test("host=/run/postgresql dbname=aimee", &tcp) ==
              0 &&
          tcp == 0);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "host=192.0.2.10 sslmode=verify-full dbname=aimee", &tcp) == 0 &&
          tcp == 1);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "host=db.example.test hostaddr=192.0.2.10 sslmode=verify-full dbname=aimee", &tcp) ==
              0 &&
          tcp == 1);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "host=db.example.test sslmode=verify-full dbname=aimee", &tcp) != 0);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "host=192.0.2.10 sslmode=require dbname=aimee", &tcp) != 0);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "host=192.0.2.10,192.0.2.11 sslmode=verify-full dbname=aimee", &tcp) != 0);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "host=/run/postgresql hostaddr=127.0.0.1 dbname=aimee", &tcp) != 0);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "hostaddr=127.0.0.1 sslmode=verify-full dbname=aimee", &tcp) != 0);
   assert(db2_vault_operator_conninfo_allowed_for_test(
              "host=db.example.test hostaddr=address.example sslmode=verify-full dbname=aimee",
              &tcp) != 0);
   assert(db2_vault_operator_runtime_open_with_vtable(&runtime, "postgres://operator", &ops, NULL,
                                                      error, sizeof(error)) == 0);
   base(1);
   db2_vault_provider_status_t local = DB2_VAULT_PROVIDER_AVAILABLE_SEALED;
   assert(db2_vault_operator_runtime_status(&runtime, provider, &local, &status) == 0);
   assert(status.state == DB2_VAULT_STATE_SEALED_IDLE &&
          status.remediation == DB2_VAULT_REMEDIATION_UNSEAL);
   base(1);
   g_value.operation_present = 1;
   g_value.operation_state = DB2_VAULT_OPERATION_PROMOTED;
   g_value.operation_seal_epoch = 7;
   g_value.operation_fence = 9;
   g_value.old_generation = 0;
   g_value.new_generation = 1;
   local = DB2_VAULT_PROVIDER_UNAVAILABLE;
   assert(db2_vault_operator_runtime_status(&runtime, provider, &local, &status) == 0);
   assert(status.state == DB2_VAULT_STATE_RESUME_REQUIRED &&
          status.remediation == DB2_VAULT_REMEDIATION_BACKEND &&
          status.snapshot.operation_id[15] == 15);
   base(0);
   local = DB2_VAULT_PROVIDER_AVAILABLE_UNSEALED;
   assert(db2_vault_operator_runtime_status(&runtime, provider, &local, &status) == 0 &&
          status.state == DB2_VAULT_STATE_OPERATIONAL);
   base(1);
   g_motion = 1;
   local = DB2_VAULT_PROVIDER_AVAILABLE_SEALED;
   assert(db2_vault_operator_runtime_status(&runtime, provider, &local, &status) ==
          DB2_VAULT_OPERATOR_INTEGRITY);
   assert(g_reads == 6 && status.state == DB2_VAULT_STATE_INTEGRITY_FAILURE);
   base(1);
   g_value.operation_present = 1;
   g_value.operation_state = DB2_VAULT_OPERATION_COMPLETED;
   g_value.operation_seal_epoch = 7;
   g_value.operation_fence = 9;
   g_value.old_generation = 2;
   g_value.new_generation = 3;
   g_null_mismatch = 1;
   assert(db2_vault_operator_runtime_status(&runtime, provider, &local, &status) ==
          DB2_VAULT_OPERATOR_INTEGRITY);
   assert(g_closed == 1 && !runtime.connection);
   db2_vault_operator_runtime_close(&runtime);

   assert(db2_vault_operator_runtime_open_with_vtable(&runtime, "postgres://operator", &ops, NULL,
                                                      error, sizeof(error)) == 0);
   base(1);
   g_database_integrity = 1;
   assert(db2_vault_operator_runtime_status(&runtime, provider, &local, &status) ==
          DB2_VAULT_OPERATOR_INTEGRITY);
   assert(g_closed == 2 && !runtime.connection);
   db2_vault_operator_runtime_close(&runtime);

   assert(db2_vault_operator_runtime_open_with_vtable(&runtime, "postgres://operator", &ops, NULL,
                                                      error, sizeof(error)) == 0);
   base(1);
   g_value.operation_present = 1;
   g_value.operation_state = DB2_VAULT_OPERATION_PROMOTED;
   g_value.operation_seal_epoch = 7;
   g_value.operation_fence = 9;
   g_value.old_generation = INT64_MAX;
   g_value.new_generation = INT64_MAX;
   assert(db2_vault_operator_runtime_status(&runtime, provider, &local, &status) ==
          DB2_VAULT_OPERATOR_INTEGRITY);
   assert(g_closed == 3 && !runtime.connection);
   db2_vault_operator_runtime_close(&runtime);
   puts("vault operator status runtime tests passed");
   return 0;
}
