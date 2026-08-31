#include "management_status_runtime.h"

#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

static void set_error(char *out, size_t cap, const char *message)
{
   if (out && cap)
      snprintf(out, cap, "%s", message);
}

static int bool_text(const char *value, int *out)
{
   if (!value || !out)
      return -1;
   if ((value[0] == 't' || value[0] == '1') && value[1] == '\0')
      *out = 1;
   else if ((value[0] == 'f' || value[0] == '0') && value[1] == '\0')
      *out = 0;
   else
      return -1;
   return 0;
}

static int one_true(void *connection, const char *sql, char *errbuf, size_t errlen)
{
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(connection, sql, errbuf, errlen);
   if (!stmt)
      return -1;
   int value = 0;
   aimee_pg_step_t first = aimee_pg_step(stmt, errbuf, errlen);
   int ok = first == AIMEE_PG_ROW && !aimee_pg_column_is_null(stmt, 0) &&
            bool_text(aimee_pg_column_text(stmt, 0), &value) == 0 && value;
   aimee_pg_step_t second = ok ? aimee_pg_step(stmt, errbuf, errlen) : AIMEE_PG_ERR;
   aimee_pg_finalize(stmt);
   return ok && second == AIMEE_PG_DONE ? 0 : -1;
}

static int pre_role_assert(void *connection, char *errbuf, size_t errlen)
{
   static const char sql[] =
       "SELECT (session_user='aimee_kb_status_login' "
       "AND current_user=session_user AND l.rolcanlogin AND NOT l.rolinherit "
       "AND NOT l.rolsuper AND NOT l.rolbypassrls AND NOT l.rolcreatedb "
       "AND NOT l.rolcreaterole AND NOT l.rolreplication "
       "AND pg_catalog.current_setting('search_path')='pg_catalog, pg_temp' "
       "AND pg_catalog.current_setting('row_security')='on' "
       "AND NOT pg_catalog.has_database_privilege(session_user,current_database(),'CREATE') "
       "AND NOT pg_catalog.has_schema_privilege(session_user,'public','CREATE') "
       "AND NOT pg_catalog.has_schema_privilege('public','public','CREATE') "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n "
       "ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relowner=l.oid) "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n "
       "ON n.oid=p.pronamespace WHERE n.nspname='public' AND p.proowner=l.oid) "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_default_acl d WHERE d.defaclrole=l.oid) "
       "AND pg_catalog.pg_has_role(session_user,'aimee_kb_status','MEMBER') "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_auth_members m "
       "JOIN pg_catalog.pg_roles g "
       "ON g.oid=m.roleid WHERE m.member=l.oid AND g.rolname<>'aimee_kb_status')) "
       "AS pre_role_ok FROM pg_catalog.pg_roles l WHERE l.rolname=session_user";
   return one_true(connection, sql, errbuf, errlen);
}

static int post_role_assert(void *connection, char *errbuf, size_t errlen)
{
   static const char sql[] =
       "SELECT (session_user='aimee_kb_status_login' "
       "AND current_user='aimee_kb_status' AND l.rolcanlogin AND NOT l.rolinherit "
       "AND NOT l.rolsuper AND NOT l.rolbypassrls AND NOT l.rolcreatedb "
       "AND NOT l.rolcreaterole AND NOT l.rolreplication "
       "AND pg_catalog.current_setting('search_path')='pg_catalog, pg_temp' "
       "AND pg_catalog.current_setting('row_security')='on' "
       "AND NOT pg_catalog.has_database_privilege(session_user,current_database(),'CREATE') "
       "AND NOT pg_catalog.has_schema_privilege(session_user,'public','CREATE') "
       "AND NOT pg_catalog.has_schema_privilege('public','public','CREATE') "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n "
       "ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relowner IN (l.oid,s.oid)) "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n "
       "ON n.oid=p.pronamespace WHERE n.nspname='public' AND p.proowner IN (l.oid,s.oid)) "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_default_acl d "
       "WHERE d.defaclrole IN (l.oid,s.oid)) "
       "AND pg_catalog.pg_has_role(session_user,'aimee_kb_status','MEMBER') "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_auth_members m "
       "JOIN pg_catalog.pg_roles g "
       "ON g.oid=m.roleid WHERE m.member=l.oid AND g.rolname<>'aimee_kb_status') "
       "AND NOT s.rolcanlogin AND NOT s.rolinherit AND NOT s.rolsuper "
       "AND NOT s.rolbypassrls AND NOT s.rolcreatedb AND NOT s.rolcreaterole "
       "AND NOT s.rolreplication "
       "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_auth_members m WHERE m.member=s.oid)) "
       "AS post_role_ok FROM pg_catalog.pg_roles l CROSS JOIN pg_catalog.pg_roles s "
       "WHERE l.rolname=session_user AND s.rolname='aimee_kb_status'";
   return one_true(connection, sql, errbuf, errlen);
}

int db2_management_status_runtime_open(db2_management_status_runtime_t *runtime,
                                       const char *conninfo, char *errbuf, size_t errlen)
{
   if (!runtime || !conninfo || !conninfo[0])
   {
      set_error(errbuf, errlen, "management status database configuration is missing");
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   }
   memset(runtime, 0, sizeof(*runtime));
   runtime->connection = aimee_pg_open(conninfo, errbuf, errlen);
   if (!runtime->connection)
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   if (aimee_pg_exec(runtime->connection, "SET search_path = pg_catalog, pg_temp", errbuf,
                     errlen) != 0 ||
       aimee_pg_exec(runtime->connection, "SET row_security = on", errbuf, errlen) != 0)
   {
      set_error(errbuf, errlen, "management status database session hardening failed");
      goto fail;
   }
   if (pre_role_assert(runtime->connection, errbuf, errlen) != 0)
   {
      set_error(errbuf, errlen, "management status login role assertion failed");
      goto fail;
   }
   if (aimee_pg_exec(runtime->connection, "SET ROLE aimee_kb_status", errbuf, errlen) != 0 ||
       post_role_assert(runtime->connection, errbuf, errlen) != 0)
   {
      set_error(errbuf, errlen, "management status effective role assertion failed");
      goto fail;
   }
   return DB2_MANAGEMENT_STATUS_RUNTIME_OK;

fail:
   aimee_pg_close(runtime->connection);
   memset(runtime, 0, sizeof(*runtime));
   return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
}

void db2_management_status_runtime_close(db2_management_status_runtime_t *runtime)
{
   if (!runtime)
      return;
   if (runtime->connection &&
       (runtime->transaction_active || aimee_pg_in_transaction(runtime->connection)))
   {
      char ignored[128] = "";
      (void)aimee_pg_exec(runtime->connection, "ROLLBACK", ignored, sizeof(ignored));
   }
   if (runtime->connection)
      aimee_pg_close(runtime->connection);
   memset(runtime, 0, sizeof(*runtime));
}

static int bounded(const char *value, size_t min, size_t max)
{
   if (!value)
      return 0;
   size_t n = strnlen(value, max + 1);
   return n >= min && n <= max;
}

static int printable(const char *value, size_t min, size_t max)
{
   if (!bounded(value, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
}

static int lower_hex(const char *value, size_t min, size_t max)
{
   if (!bounded(value, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

static int token(const char *value, size_t min, size_t max)
{
   if (!bounded(value, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '.' || *p == '_' || *p == '-'))
         return 0;
   return 1;
}

int db2_management_status_runtime_lookup(db2_management_status_runtime_t *runtime,
                                         const char *issuer, const char *serial_norm,
                                         const char *fingerprint, const char *target,
                                         const char *purpose, int64_t *generation,
                                         char *target_fingerprint, size_t target_fingerprint_len)
{
   if (generation)
      *generation = 0;
   if (target_fingerprint && target_fingerprint_len)
      target_fingerprint[0] = '\0';
   if (!runtime || !runtime->connection || runtime->transaction_active ||
       aimee_pg_in_transaction(runtime->connection) || !printable(issuer, 1, 600) ||
       !lower_hex(serial_norm, 1, 128) || !lower_hex(fingerprint, 64, 64) ||
       !token(target, 1, 127) || !purpose ||
       (strcmp(purpose, "management.health.v1") != 0 &&
        strcmp(purpose, "management.action.v1") != 0) ||
       !generation || !target_fingerprint || target_fingerprint_len < 65)
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;

   char error[256] = "";
   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(runtime->connection,
                        "SELECT revocation_generation,target_mgmt_fingerprint FROM "
                        "public.kb_management_status_lookup(?1,?2,?3,?4,?5)",
                        error, sizeof(error));
   if (!stmt)
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   if (aimee_pg_bind_text(stmt, "?1", issuer) != 0 ||
       aimee_pg_bind_text(stmt, "?2", serial_norm) != 0 ||
       aimee_pg_bind_text(stmt, "?3", fingerprint) != 0 ||
       aimee_pg_bind_text(stmt, "?4", target) != 0 || aimee_pg_bind_text(stmt, "?5", purpose) != 0)
   {
      aimee_pg_finalize(stmt);
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   }
   aimee_pg_step_t first = aimee_pg_step(stmt, error, sizeof(error));
   int rc = DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   const char *sqlstate = first == AIMEE_PG_ERR ? aimee_pg_sqlstate(stmt) : NULL;
   if (first == AIMEE_PG_ERR && sqlstate && strcmp(sqlstate, "28000") == 0)
      rc = DB2_MANAGEMENT_STATUS_RUNTIME_DENIED;
   else if (first == AIMEE_PG_DONE)
      rc = DB2_MANAGEMENT_STATUS_RUNTIME_DENIED;
   else if (first == AIMEE_PG_ROW)
   {
      int64_t found_generation = aimee_pg_column_int64(stmt, 0);
      const char *found_fingerprint = aimee_pg_column_text(stmt, 1);
      if (found_generation < 1 || !lower_hex(found_fingerprint, 64, 64))
         rc = DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY;
      else
      {
         char stable_fingerprint[65];
         memcpy(stable_fingerprint, found_fingerprint, sizeof(stable_fingerprint));
         if (aimee_pg_step(stmt, error, sizeof(error)) != AIMEE_PG_DONE)
            rc = DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY;
         else
         {
            *generation = found_generation;
            memcpy(target_fingerprint, stable_fingerprint, sizeof(stable_fingerprint));
            rc = DB2_MANAGEMENT_STATUS_RUNTIME_OK;
         }
      }
   }
   aimee_pg_finalize(stmt);
   if (rc != DB2_MANAGEMENT_STATUS_RUNTIME_OK)
   {
      *generation = 0;
      target_fingerprint[0] = '\0';
   }
   return rc;
}

int db2_management_status_runtime_action_checkpoint(
    db2_management_status_runtime_t *runtime, const char *peer_issuer, const char *peer_serial,
    const char *peer_fingerprint, const char *target, const char *caller_issuer,
    const char *caller_serial, const char *caller_fingerprint, int64_t staple_generation,
    int *revoked, int64_t *generation)
{
   if (revoked)
      *revoked = 0;
   if (generation)
      *generation = 0;
   if (!runtime || !runtime->connection || runtime->transaction_active ||
       aimee_pg_in_transaction(runtime->connection) || !printable(peer_issuer, 1, 600) ||
       !lower_hex(peer_serial, 1, 128) || !lower_hex(peer_fingerprint, 64, 64) ||
       !token(target, 1, 127) || !printable(caller_issuer, 1, 600) ||
       !lower_hex(caller_serial, 1, 128) || !lower_hex(caller_fingerprint, 64, 64) ||
       staple_generation < 1 || !revoked || !generation)
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   char error[256] = "";
   aimee_pg_stmt_t *stmt =
       aimee_pg_prepare(runtime->connection,
                        "SELECT revoked,generation FROM public.kb_management_action_checkpoint("
                        "?1,?2,?3,?4,?5,?6,?7,?8)",
                        error, sizeof(error));
   if (!stmt)
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   if (aimee_pg_bind_text(stmt, "?1", peer_issuer) || aimee_pg_bind_text(stmt, "?2", peer_serial) ||
       aimee_pg_bind_text(stmt, "?3", peer_fingerprint) || aimee_pg_bind_text(stmt, "?4", target) ||
       aimee_pg_bind_text(stmt, "?5", caller_issuer) ||
       aimee_pg_bind_text(stmt, "?6", caller_serial) ||
       aimee_pg_bind_text(stmt, "?7", caller_fingerprint) ||
       aimee_pg_bind_int64(stmt, "?8", staple_generation))
   {
      aimee_pg_finalize(stmt);
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   }
   aimee_pg_step_t first = aimee_pg_step(stmt, error, sizeof(error));
   const char *state = first == AIMEE_PG_ERR ? aimee_pg_sqlstate(stmt) : NULL;
   int rc = DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   if (first == AIMEE_PG_ERR && state && !strcmp(state, "28000"))
      rc = DB2_MANAGEMENT_STATUS_RUNTIME_DENIED;
   else if (first == AIMEE_PG_ERR && state && !strcmp(state, "23505"))
      rc = DB2_MANAGEMENT_STATUS_RUNTIME_CONFLICT;
   else if (first == AIMEE_PG_ROW)
   {
      int found_revoked = 0;
      int64_t found_generation = aimee_pg_column_int64(stmt, 1);
      if (bool_text(aimee_pg_column_text(stmt, 0), &found_revoked) || found_generation < 1 ||
          aimee_pg_step(stmt, error, sizeof(error)) != AIMEE_PG_DONE)
         rc = DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY;
      else
      {
         *revoked = found_revoked;
         *generation = found_generation;
         rc = DB2_MANAGEMENT_STATUS_RUNTIME_OK;
      }
   }
   else if (first == AIMEE_PG_DONE)
      rc = DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY;
   aimee_pg_finalize(stmt);
   if (rc != DB2_MANAGEMENT_STATUS_RUNTIME_OK)
   {
      *revoked = 0;
      *generation = 0;
   }
   return rc;
}

static int copy_blob(aimee_pg_stmt_t *stmt, int column, unsigned char *out, size_t expected)
{
   const void *value = aimee_pg_column_blob(stmt, column);
   int size = aimee_pg_column_bytes(stmt, column);
   if (!value || size < 0 || (size_t)size != expected)
      return -1;
   memcpy(out, value, expected);
   return 0;
}

int db2_management_status_runtime_startup_begin(db2_management_status_runtime_t *runtime,
                                                db2_management_status_runtime_startup_t *out)
{
   if (!runtime || !runtime->connection || !out || runtime->transaction_active ||
       aimee_pg_in_transaction(runtime->connection))
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   memset(out, 0, sizeof(*out));
   char error[256] = "";
   if (aimee_pg_exec(runtime->connection, "BEGIN", error, sizeof(error)) != 0)
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   runtime->transaction_active = 1;
   int failure = DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       runtime->connection,
       "SELECT seal_epoch,sealed,custody_key_id,wire_key_id,public_key,enabled,version,"
       "hwm_attestation FROM public.kb_management_status_key_startup_status()",
       error, sizeof(error));
   if (!stmt)
      goto fail;
   aimee_pg_step_t first = aimee_pg_step(stmt, error, sizeof(error));
   if (first != AIMEE_PG_ROW)
   {
      if (first == AIMEE_PG_DONE)
         failure = DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY;
      aimee_pg_finalize(stmt);
      goto fail;
   }
   if (aimee_pg_column_is_null(stmt, 0) || aimee_pg_column_is_null(stmt, 1) ||
       aimee_pg_column_is_null(stmt, 2) || aimee_pg_column_is_null(stmt, 3) ||
       aimee_pg_column_is_null(stmt, 4) || aimee_pg_column_is_null(stmt, 5) ||
       aimee_pg_column_is_null(stmt, 6) || aimee_pg_column_is_null(stmt, 7))
   {
      failure = DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY;
      aimee_pg_finalize(stmt);
      goto fail;
   }
   const char *custody = aimee_pg_column_text(stmt, 2);
   const char *wire = aimee_pg_column_text(stmt, 3);
   int sealed = 0, enabled = 0;
   int64_t epoch = aimee_pg_column_int64(stmt, 0);
   int64_t version = aimee_pg_column_int64(stmt, 6);
   db2_management_status_runtime_startup_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   int valid = epoch >= 1 && version >= 1 && bounded(custody, 1, 600) && token(wire, 1, 64) &&
               bool_text(aimee_pg_column_text(stmt, 1), &sealed) == 0 &&
               bool_text(aimee_pg_column_text(stmt, 5), &enabled) == 0 &&
               copy_blob(stmt, 4, candidate.public_key, sizeof(candidate.public_key)) == 0;
   if (valid)
   {
      candidate.seal_epoch = epoch;
      candidate.sealed = sealed;
      snprintf(candidate.custody_key_id, sizeof(candidate.custody_key_id), "%s", custody);
      snprintf(candidate.wire_key_id, sizeof(candidate.wire_key_id), "%s", wire);
      candidate.enabled = enabled;
      candidate.version = version;
      /* aimee_pg_column_blob owns one per-statement decode cache. Copy each
       * bytea before asking for a different column, or the later decode frees
       * and replaces the earlier pointer. */
      int attestation_size = aimee_pg_column_bytes(stmt, 7);
      const void *attestation = aimee_pg_column_blob(stmt, 7);
      if (!attestation || attestation_size <= 0 ||
          (size_t)attestation_size > sizeof(candidate.hwm_attestation))
         valid = 0;
      else
      {
         memcpy(candidate.hwm_attestation, attestation, (size_t)attestation_size);
         candidate.hwm_attestation_len = (size_t)attestation_size;
      }
   }
   if (valid)
   {
      valid = aimee_pg_step(stmt, error, sizeof(error)) == AIMEE_PG_DONE;
   }
   aimee_pg_finalize(stmt);
   if (!valid)
   {
      failure = DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY;
      goto fail;
   }
   *out = candidate;
   return DB2_MANAGEMENT_STATUS_RUNTIME_OK;

fail:
   (void)db2_management_status_runtime_startup_end(runtime, 0);
   memset(out, 0, sizeof(*out));
   return failure;
}

int db2_management_status_runtime_startup_end(db2_management_status_runtime_t *runtime, int commit)
{
   if (!runtime || !runtime->connection || !runtime->transaction_active)
      return DB2_MANAGEMENT_STATUS_RUNTIME_ERROR;
   char error[128] = "";
   if (commit && aimee_pg_exec(runtime->connection, "COMMIT", error, sizeof(error)) == 0)
   {
      runtime->transaction_active = 0;
      return DB2_MANAGEMENT_STATUS_RUNTIME_OK;
   }
   (void)aimee_pg_exec(runtime->connection, "ROLLBACK", error, sizeof(error));
   runtime->transaction_active = 0;
   return commit ? DB2_MANAGEMENT_STATUS_RUNTIME_ERROR : DB2_MANAGEMENT_STATUS_RUNTIME_OK;
}
