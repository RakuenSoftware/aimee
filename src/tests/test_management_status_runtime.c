#include "modules/db2/c/management_status_runtime.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum
{
   MODE_NONE,
   MODE_PRE_ROLE,
   MODE_POST_ROLE,
   MODE_LOOKUP,
   MODE_CHECKPOINT,
   MODE_STARTUP,
};

struct aimee_pg_stmt
{
   int mode;
   int step;
};

static struct aimee_pg_stmt g_stmt;
static int g_closed, g_in_tx, g_rollbacks, g_commits, g_set_role;
static int g_search_path, g_row_security, g_guc_failure;
static int g_pre_ok = 1, g_post_ok = 1, g_lookup_denied, g_lookup_error, g_lookup_extra;
static int g_lookup_policy_denied;
static int g_lookup_conflict;
static int g_lookup_malformed, g_startup_malformed;
static unsigned g_bind_mask;

void *aimee_pg_open(const char *dsn, char *error, size_t error_len)
{
   (void)error;
   (void)error_len;
   return dsn && dsn[0] ? &g_stmt : NULL;
}

void aimee_pg_close(void *connection)
{
   assert(connection == &g_stmt);
   g_closed++;
}

int aimee_pg_in_transaction(void *connection)
{
   assert(connection == &g_stmt);
   return g_in_tx;
}

int aimee_pg_exec(void *connection, const char *sql, char *error, size_t error_len)
{
   (void)error;
   (void)error_len;
   assert(connection == &g_stmt);
   if (!strcmp(sql, "SET search_path = pg_catalog, pg_temp"))
   {
      g_search_path++;
      if (g_guc_failure == 1)
         return -1;
   }
   else if (!strcmp(sql, "SET row_security = on"))
   {
      g_row_security++;
      if (g_guc_failure == 2)
         return -1;
   }
   else if (!strcmp(sql, "SET ROLE aimee_kb_status"))
      g_set_role++;
   else if (!strcmp(sql, "BEGIN"))
   {
      assert(!g_in_tx);
      g_in_tx = 1;
   }
   else if (!strcmp(sql, "COMMIT"))
   {
      assert(g_in_tx);
      g_in_tx = 0;
      g_commits++;
   }
   else if (!strcmp(sql, "ROLLBACK"))
   {
      g_in_tx = 0;
      g_rollbacks++;
   }
   else
      assert(0);
   return 0;
}

aimee_pg_stmt_t *aimee_pg_prepare(void *connection, const char *sql, char *error, size_t error_len)
{
   (void)error;
   (void)error_len;
   assert(connection == &g_stmt);
   g_stmt.mode = strstr(sql, "pre_role_ok")         ? MODE_PRE_ROLE
                 : strstr(sql, "post_role_ok")      ? MODE_POST_ROLE
                 : strstr(sql, "status_lookup")     ? MODE_LOOKUP
                 : strstr(sql, "action_checkpoint") ? MODE_CHECKPOINT
                 : strstr(sql, "startup_status")    ? MODE_STARTUP
                                                    : MODE_NONE;
   assert(g_stmt.mode != MODE_NONE);
   if (g_stmt.mode == MODE_PRE_ROLE || g_stmt.mode == MODE_POST_ROLE)
   {
      assert(strstr(sql, "pg_catalog.current_setting('search_path')='pg_catalog, pg_temp'"));
      assert(strstr(sql, "pg_catalog.current_setting('row_security')='on'"));
      assert(strstr(sql, "pg_catalog.pg_auth_members"));
      assert(strstr(sql, "pg_catalog.pg_roles"));
   }
   else if (g_stmt.mode == MODE_LOOKUP)
      assert(strstr(sql, "public.kb_management_status_lookup"));
   else if (g_stmt.mode == MODE_CHECKPOINT)
      assert(strstr(sql, "public.kb_management_action_checkpoint"));
   else if (g_stmt.mode == MODE_STARTUP)
      assert(strstr(sql, "public.kb_management_status_key_startup_status"));
   g_stmt.step = 0;
   g_bind_mask = 0;
   return &g_stmt;
}

void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   assert(stmt == &g_stmt);
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *error, size_t error_len)
{
   (void)error;
   (void)error_len;
   assert(stmt == &g_stmt);
   if ((stmt->mode == MODE_LOOKUP || stmt->mode == MODE_CHECKPOINT) && g_lookup_error)
      return AIMEE_PG_ERR;
   if ((stmt->mode == MODE_LOOKUP || stmt->mode == MODE_CHECKPOINT) && g_lookup_denied)
      return AIMEE_PG_DONE;
   if (stmt->step++ == 0)
      return AIMEE_PG_ROW;
   if (stmt->mode == MODE_LOOKUP && g_lookup_extra && stmt->step == 2)
      return AIMEE_PG_ROW;
   return AIMEE_PG_DONE;
}

const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *stmt)
{
   assert(stmt == &g_stmt);
   return g_lookup_policy_denied ? "28000"
                                 : (g_lookup_conflict ? "23505" : (g_lookup_error ? "08006" : ""));
}

int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *key, const char *value)
{
   assert(stmt == &g_stmt && (stmt->mode == MODE_LOOKUP || stmt->mode == MODE_CHECKPOINT) && key &&
          value);
   int number = key[1] - '0';
   assert(key[0] == '?' && number >= 1 && number <= (stmt->mode == MODE_LOOKUP ? 5 : 7) &&
          key[2] == '\0');
   g_bind_mask |= 1u << (unsigned)(number - 1);
   return 0;
}

int aimee_pg_bind_int64(aimee_pg_stmt_t *stmt, const char *key, int64_t value)
{
   assert(stmt == &g_stmt && stmt->mode == MODE_CHECKPOINT && !strcmp(key, "?8") && value == 9);
   g_bind_mask |= 1u << 7;
   return 0;
}

int aimee_pg_column_is_null(aimee_pg_stmt_t *stmt, int column)
{
   assert(stmt == &g_stmt);
   if (stmt->mode == MODE_STARTUP && g_startup_malformed && column == 7)
      return 1;
   return 0;
}

const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int column)
{
   assert(stmt == &g_stmt);
   static char fp[65];
   memset(fp, 'a', 64);
   fp[64] = '\0';
   if (stmt->mode == MODE_PRE_ROLE)
      return g_pre_ok ? "t" : "f";
   if (stmt->mode == MODE_POST_ROLE)
      return g_post_ok ? "t" : "f";
   if (stmt->mode == MODE_LOOKUP)
      return column == 1 ? (g_lookup_malformed ? "BAD" : fp) : "";
   if (stmt->mode == MODE_CHECKPOINT)
      return column == 0 ? "t" : "";
   if (stmt->mode == MODE_STARTUP)
   {
      if (column == 1)
         return "f";
      if (column == 2)
         return "kms:status";
      if (column == 3)
         return "p5-status-v1-deadbeef";
      if (column == 5)
         return "t";
   }
   return "";
}

int64_t aimee_pg_column_int64(aimee_pg_stmt_t *stmt, int column)
{
   assert(stmt == &g_stmt);
   if (stmt->mode == MODE_LOOKUP)
      return g_lookup_malformed ? 0 : 9;
   if (stmt->mode == MODE_CHECKPOINT)
      return column == 1 ? 12 : 0;
   assert(stmt->mode == MODE_STARTUP);
   return column == 0 ? 7 : 2;
}

const void *aimee_pg_column_blob(aimee_pg_stmt_t *stmt, int column)
{
   assert(stmt == &g_stmt && stmt->mode == MODE_STARTUP);
   static unsigned char one_column_cache[64];
   if (column != 4 && column != 7)
      return NULL;
   memset(one_column_cache, column == 4 ? 1 : 2, sizeof(one_column_cache));
   return one_column_cache;
}

int aimee_pg_column_bytes(aimee_pg_stmt_t *stmt, int column)
{
   assert(stmt == &g_stmt && stmt->mode == MODE_STARTUP);
   (void)aimee_pg_column_blob(stmt, column);
   return column == 4 ? 32 : (column == 7 ? 64 : 0);
}

static void reset_connection_counts(void)
{
   g_closed = g_in_tx = g_rollbacks = g_commits = g_set_role = 0;
   g_search_path = g_row_security = g_guc_failure = 0;
   g_pre_ok = g_post_ok = 1;
}

int main(void)
{
   char error[128] = "";
   db2_management_status_runtime_t runtime;
   assert(db2_management_status_runtime_open(&runtime, "", error, sizeof(error)) < 0);

   reset_connection_counts();
   g_guc_failure = 1;
   assert(db2_management_status_runtime_open(&runtime, "postgres://status", error, sizeof(error)) <
          0);
   assert(g_closed == 1 && g_search_path == 1 && g_row_security == 0 && g_set_role == 0 &&
          !runtime.connection);

   reset_connection_counts();
   g_guc_failure = 2;
   assert(db2_management_status_runtime_open(&runtime, "postgres://status", error, sizeof(error)) <
          0);
   assert(g_closed == 1 && g_search_path == 1 && g_row_security == 1 && g_set_role == 0 &&
          !runtime.connection);

   reset_connection_counts();
   g_pre_ok = 0;
   assert(db2_management_status_runtime_open(&runtime, "postgres://status", error, sizeof(error)) <
          0);
   assert(g_closed == 1 && g_search_path == 1 && g_row_security == 1 && g_set_role == 0 &&
          !runtime.connection);

   reset_connection_counts();
   g_post_ok = 0;
   assert(db2_management_status_runtime_open(&runtime, "postgres://status", error, sizeof(error)) <
          0);
   assert(g_closed == 1 && g_search_path == 1 && g_row_security == 1 && g_set_role == 1 &&
          !runtime.connection);

   reset_connection_counts();
   assert(db2_management_status_runtime_open(&runtime, "postgres://status", error, sizeof(error)) ==
          0);
   assert(g_search_path == 1 && g_row_security == 1 && g_set_role == 1 && runtime.connection);

   char caller_fp[65], target_fp[65] = "";
   memset(caller_fp, 'b', 64);
   caller_fp[64] = '\0';
   int64_t generation = 0;
   assert(db2_management_status_runtime_lookup(
              &runtime, "issuer", "01", caller_fp, "server-1", "management.health.v1", &generation,
              target_fp, sizeof(target_fp)) == DB2_MANAGEMENT_STATUS_RUNTIME_OK);
   assert(g_bind_mask == 0x1fu && generation == 9 && strlen(target_fp) == 64);
   assert(db2_management_status_runtime_lookup(
              &runtime, "issuer", "01", caller_fp, "server-1", "management.action.v1", &generation,
              target_fp, sizeof(target_fp)) == DB2_MANAGEMENT_STATUS_RUNTIME_OK);

   g_lookup_denied = 1;
   assert(db2_management_status_runtime_lookup(
              &runtime, "issuer", "01", caller_fp, "server-1", "management.health.v1", &generation,
              target_fp, sizeof(target_fp)) == DB2_MANAGEMENT_STATUS_RUNTIME_DENIED);
   assert(generation == 0 && !target_fp[0]);
   g_lookup_denied = 0;
   g_lookup_error = 1;
   assert(db2_management_status_runtime_lookup(
              &runtime, "issuer", "01", caller_fp, "server-1", "management.health.v1", &generation,
              target_fp, sizeof(target_fp)) == DB2_MANAGEMENT_STATUS_RUNTIME_ERROR);
   g_lookup_error = 0;
   g_lookup_policy_denied = 1;
   g_lookup_error = 1;
   assert(db2_management_status_runtime_lookup(
              &runtime, "issuer", "01", caller_fp, "server-1", "management.health.v1", &generation,
              target_fp, sizeof(target_fp)) == DB2_MANAGEMENT_STATUS_RUNTIME_DENIED);
   assert(generation == 0 && !target_fp[0]);
   g_lookup_error = 0;
   g_lookup_policy_denied = 0;
   g_lookup_malformed = 1;
   assert(db2_management_status_runtime_lookup(
              &runtime, "issuer", "01", caller_fp, "server-1", "management.health.v1", &generation,
              target_fp, sizeof(target_fp)) == DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY);
   g_lookup_malformed = 0;
   g_lookup_extra = 1;
   assert(db2_management_status_runtime_lookup(
              &runtime, "issuer", "01", caller_fp, "server-1", "management.health.v1", &generation,
              target_fp, sizeof(target_fp)) == DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY);
   g_lookup_extra = 0;
   assert(db2_management_status_runtime_lookup(&runtime, "issuer", "01", "bad", "server-1",
                                               "management.health.v1", &generation, target_fp,
                                               sizeof(target_fp)) < 0);

   int revoked = 0;
   assert(db2_management_status_runtime_action_checkpoint(
              &runtime, "server-issuer", "02", caller_fp, "server-1", "caller-issuer", "01",
              caller_fp, 9, &revoked, &generation) == DB2_MANAGEMENT_STATUS_RUNTIME_OK);
   assert(g_bind_mask == 0xffu && revoked == 1 && generation == 12);
   g_lookup_policy_denied = 1;
   g_lookup_error = 1;
   assert(db2_management_status_runtime_action_checkpoint(
              &runtime, "server-issuer", "02", caller_fp, "server-1", "caller-issuer", "01",
              caller_fp, 9, &revoked, &generation) == DB2_MANAGEMENT_STATUS_RUNTIME_DENIED);
   assert(!revoked && !generation);
   g_lookup_policy_denied = 0;
   g_lookup_error = 0;
   g_lookup_conflict = 1;
   g_lookup_error = 1;
   assert(db2_management_status_runtime_action_checkpoint(
              &runtime, "server-issuer", "02", caller_fp, "server-1", "caller-issuer", "01",
              caller_fp, 9, &revoked, &generation) == DB2_MANAGEMENT_STATUS_RUNTIME_CONFLICT);
   g_lookup_conflict = 0;
   g_lookup_error = 0;

   db2_management_status_runtime_startup_t startup;
   assert(db2_management_status_runtime_startup_begin(&runtime, &startup) == 0);
   assert(runtime.transaction_active && startup.seal_epoch == 7 && !startup.sealed &&
          startup.enabled && startup.version == 2 && startup.hwm_attestation_len == 64 &&
          !strcmp(startup.custody_key_id, "kms:status") &&
          !strcmp(startup.wire_key_id, "p5-status-v1-deadbeef"));
   for (size_t i = 0; i < sizeof(startup.public_key); ++i)
      assert(startup.public_key[i] == 1);
   for (size_t i = 0; i < startup.hwm_attestation_len; ++i)
      assert(startup.hwm_attestation[i] == 2);
   assert(db2_management_status_runtime_startup_begin(&runtime, &startup) < 0);
   assert(db2_management_status_runtime_startup_end(&runtime, 1) == 0 && g_commits == 1);

   g_startup_malformed = 1;
   assert(db2_management_status_runtime_startup_begin(&runtime, &startup) ==
          DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY);
   assert(!runtime.transaction_active && g_rollbacks == 1);
   g_startup_malformed = 0;

   assert(db2_management_status_runtime_startup_begin(&runtime, &startup) == 0);
   db2_management_status_runtime_close(&runtime);
   assert(g_closed == 1 && g_rollbacks == 2 && !g_in_tx && !runtime.connection);
   puts("management_status_runtime: all tests passed");
   return 0;
}
