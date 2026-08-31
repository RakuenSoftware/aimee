#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vault_operator_rewrap_runtime.h"

#include <libpq-fe.h>
#include <openssl/crypto.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define D3B_DB_MS 2000
#define API       "aimee_kb_vault_orchestrator_api."
#define ORCHESTRATOR_FUNCTIONS_SQL                                                                 \
   "ARRAY['aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()',"                   \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_dispatch(text,text)',"                       \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_reserve(text,text,text,bigint,bigint)',"     \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_active()',"                                  \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed(text,text,text)',"                 \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed_active(text,text)',"               \
   "'aimee_kb_vault_orchestrator_api.org_vault_current_check_page(bytea,integer)',"                \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_open_completed(text,text,text,bigint,"       \
   "bigint,bytea,bytea,bytea)',"                                                                   \
   "'aimee_kb_vault_orchestrator_api.org_vault_open_idle(text,text,bigint,bigint,bigint)',"        \
   "'aimee_kb_vault_orchestrator_api.org_vault_open_event(text)',"                                 \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_snapshot(text)',"                            \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_record_prepared(text,bigint,bytea,bytea)',"  \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_secret_page(text,bigint,bigint,integer)',"   \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_check_page(text,bigint,bytea,integer)',"     \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_stage_dek(text,bigint,bigint,text,text,"     \
   "text,bigint,bytea,bytea)',"                                                                    \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_stage_check(text,bigint,text,bytea,bytea)'," \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_inventory_summary(text,bigint)',"            \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_stage_finish(text,bigint,bigint,bigint,"     \
   "bytea)',"                                                                                      \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_mark_committing(text,bigint)',"              \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_mark_resealed(text,bigint,bytea)',"          \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_promote(text,bigint)',"                      \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_abort(text,bigint,text)',"                   \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_recovery_required(text,bigint,text)',"       \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_verify_summary(text,bigint)',"               \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_verify_secret_page(text,bigint,bigint,"      \
   "integer)',"                                                                                    \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_verify_check_page(text,bigint,bytea,"        \
   "integer)',"                                                                                    \
   "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_complete(text,bigint,bytea,bytea,bytea)']"

enum
{
   TX_GENERAL = 1,
   TX_STAGING,
   TX_STAGE_DONE,
   TX_SINGLE_DONE,
   TX_PROMOTE_DONE,
   TX_VERIFY_SECRET,
   TX_VERIFY_CHECK,
   TX_VERIFY_CONSUMED,
   TX_ACKED,
   TX_COMPLETE,
   TX_FAILED
};

struct db2_vault_rewrap_tx
{
   db2_vault_operator_runtime_t *runtime;
   pthread_t owner;
   int phase, kind;
   uint8_t operation_id[16];
   int64_t fence, expected_secrets, expected_checks, consumed_secrets, consumed_checks;
   int64_t last_secret;
   db2_vault_rewrap_cursor_t cursor;
   int secret_exhausted, check_exhausted;
   uint8_t receipt_digest[32], inventory_digest[32], stage_digest[32];
};

static pthread_mutex_t binding_mutex = PTHREAD_MUTEX_INITIALIZER;
static db2_vault_operator_runtime_t *bound_runtime;
static PGconn *uncertain_connection;

static pthread_mutex_t *runtime_mutex(db2_vault_operator_runtime_t *r)
{
   return (pthread_mutex_t *)(void *)r->mutex_storage;
}

static int64_t mono_ms(void)
{
   struct timespec t;
   return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ? (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000
                                                  : -1;
}

static int64_t deadline(void)
{
   int64_t n = mono_ms();
   if (n < 0 || n > INT64_MAX - D3B_DB_MS)
      return -1;
   int64_t end = db2_vault_reseal_deadline_ms(D3B_DB_MS);
   return end > n && end <= n + D3B_DB_MS ? end : -1;
}

static int lock_until(pthread_mutex_t *m, int64_t end)
{
   for (;;)
   {
      int rc = pthread_mutex_trylock(m);
      if (!rc)
         return 0;
      if (rc != EBUSY || mono_ms() < 0 || mono_ms() >= end)
         return -1;
      struct timespec p = {.tv_nsec = 1000000};
      (void)nanosleep(&p, NULL);
   }
}

static int wait_socket(PGconn *c, short events, int64_t end)
{
   for (;;)
   {
      int64_t n = mono_ms();
      if (n < 0 || n >= end)
         return -1;
      struct pollfd p = {.fd = PQsocket(c), .events = events};
      int64_t left = end - n;
      int rc = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
      if (rc < 0 && errno == EINTR)
         continue;
      return rc == 1 && !(p.revents & (POLLERR | POLLNVAL)) && (p.revents & (events | POLLHUP))
                 ? 0
                 : -1;
   }
}

static db2_vault_rewrap_result_t sql_error(PGresult *r)
{
   const char *s = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : NULL;
   return db2_vault_rewrap_classify_sqlstate(s);
}
static void mark_uncertain(PGconn *c)
{
   pthread_mutex_lock(&binding_mutex);
   if (bound_runtime && bound_runtime->connection == c)
      uncertain_connection = c;
   pthread_mutex_unlock(&binding_mutex);
}

static int connection_uncertain(PGconn *c)
{
   pthread_mutex_lock(&binding_mutex);
   int uncertain = uncertain_connection == c;
   pthread_mutex_unlock(&binding_mutex);
   return uncertain;
}

static db2_vault_rewrap_result_t query(PGconn *c, const char *sql, int n, const Oid *types,
                                       const char *const *values, const int *lengths,
                                       const int *formats, PGresult **out)
{
   *out = NULL;
   int64_t end = deadline();
   if (end < 0 || !c || connection_uncertain(c) || PQstatus(c) != CONNECTION_OK ||
       PQtransactionStatus(c) == PQTRANS_UNKNOWN)
   {
      if (c)
         mark_uncertain(c);
      return DB2_VAULT_REWRAP_TRANSIENT;
   }
   if (!PQsendQueryParams(c, sql, n, types, values, lengths, formats, 1))
   {
      mark_uncertain(c);
      return DB2_VAULT_REWRAP_TRANSIENT;
   }
   while (PQflush(c) == 1)
      if (wait_socket(c, POLLOUT, end) != 0)
      {
         mark_uncertain(c);
         return DB2_VAULT_REWRAP_TRANSIENT;
      }
   while (PQisBusy(c))
   {
      if (wait_socket(c, POLLIN, end) != 0 || !PQconsumeInput(c))
      {
         mark_uncertain(c);
         return DB2_VAULT_REWRAP_TRANSIENT;
      }
   }
   PGresult *r = PQgetResult(c);
   if (!r)
   {
      mark_uncertain(c);
      return DB2_VAULT_REWRAP_TRANSIENT;
   }
   ExecStatusType st = PQresultStatus(r);
   if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
   {
      const char *state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
      int poison = state && (!strncmp(state, "08", 2) || !strncmp(state, "57", 2));
      db2_vault_rewrap_result_t rc = sql_error(r);
      PQclear(r);
      while ((r = PQgetResult(c)) != NULL)
         PQclear(r);
      if (poison)
      {
         mark_uncertain(c);
         return DB2_VAULT_REWRAP_TRANSIENT;
      }
      return rc;
   }
   PGresult *surplus = PQgetResult(c);
   if (surplus)
   {
      PQclear(surplus);
      PQclear(r);
      mark_uncertain(c);
      return DB2_VAULT_REWRAP_INTEGRITY;
   }
   *out = r;
   return DB2_VAULT_REWRAP_OK;
}

static db2_vault_rewrap_result_t command(PGconn *c, const char *sql)
{
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc = query(c, sql, 0, NULL, NULL, NULL, NULL, &r);
   if (rc == DB2_VAULT_REWRAP_OK && (PQntuples(r) != 0 || PQnfields(r) != 0))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   PQclear(r);
   return rc;
}

static uint64_t load64(const unsigned char *p)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < 8; i++)
      v = (v << 8) | p[i];
   return v;
}
static int col_i64(PGresult *r, int row, int col, int64_t *out)
{
   if (PQgetisnull(r, row, col) || PQgetlength(r, row, col) != 8)
      return -1;
   *out = (int64_t)load64((unsigned char *)PQgetvalue(r, row, col));
   return 0;
}
static int col_bool(PGresult *r, int row, int col, int *out)
{
   if (PQgetisnull(r, row, col) || PQgetlength(r, row, col) != 1)
      return -1;
   unsigned char v = *(unsigned char *)PQgetvalue(r, row, col);
   if (v > 1)
      return -1;
   *out = v;
   return 0;
}
static int col_text(PGresult *r, int row, int col, char *out, size_t cap)
{
   if (PQgetisnull(r, row, col))
      return -1;
   int n = PQgetlength(r, row, col);
   if (n < 0 || (size_t)n >= cap || memchr(PQgetvalue(r, row, col), 0, (size_t)n))
      return -1;
   memcpy(out, PQgetvalue(r, row, col), (size_t)n);
   out[n] = 0;
   return 0;
}
static int col_blob(PGresult *r, int row, int col, void *out, size_t n)
{
   if (PQgetisnull(r, row, col) || PQgetlength(r, row, col) != (int)n)
      return -1;
   if (n)
      memcpy(out, PQgetvalue(r, row, col), n);
   return 0;
}
static int state_parse(PGresult *r, int row, int col, db2_vault_rewrap_state_t *out)
{
   static const char *names[] = {"preparing",         "custody_prepared", "wraps_staged",
                                 "reseal_committing", "resealed",         "promoted",
                                 "completed",         "aborted",          "recovery_required"};
   char s[32];
   if (col_text(r, row, col, s, sizeof(s)))
      return -1;
   for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      if (!strcmp(s, names[i]))
      {
         *out = i;
         return 0;
      }
   return -1;
}
static int op_hex(const uint8_t op[16], char out[33])
{
   return op ? db2_vault_reseal_operation_id_to_hex(op, out) : -1;
}
static int failure_valid(const char *s)
{
   size_t n = s ? strlen(s) : 0;
   if (!n)
      return 1;
   if (n > 64 || s[0] < 'a' || s[0] > 'z')
      return 0;
   for (size_t i = 1; i < n; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
         return 0;
   return 1;
}
static int snapshot_shape(const db2_vault_rewrap_snapshot_t *o)
{
   int nofail = !o->failure_class[0] && !o->has_failure_from_state;
   int none =
       !o->has_receipt && !o->has_inventory && !o->has_stage && !o->secret_count && !o->check_count;
   int prepared =
       o->has_receipt && !o->has_inventory && !o->has_stage && !o->secret_count && !o->check_count;
   int staged = o->has_receipt && o->has_inventory && o->has_stage;
   switch (o->state)
   {
   case DB2_VAULT_REWRAP_PREPARING:
      return nofail && none;
   case DB2_VAULT_REWRAP_CUSTODY_PREPARED:
      return nofail && prepared;
   case DB2_VAULT_REWRAP_WRAPS_STAGED:
   case DB2_VAULT_REWRAP_RESEAL_COMMITTING:
   case DB2_VAULT_REWRAP_RESEALED:
   case DB2_VAULT_REWRAP_PROMOTED:
   case DB2_VAULT_REWRAP_COMPLETED:
      return nofail && staged;
   case DB2_VAULT_REWRAP_ABORTED:
      return o->failure_class[0] && !o->has_failure_from_state && (none || prepared || staged);
   case DB2_VAULT_REWRAP_RECOVERY_REQUIRED:
      if (!o->failure_class[0] || !o->has_failure_from_state)
         return 0;
      if (o->failure_from_state == DB2_VAULT_REWRAP_PREPARING)
         return none;
      if (o->failure_from_state == DB2_VAULT_REWRAP_CUSTODY_PREPARED)
         return prepared;
      return o->failure_from_state >= DB2_VAULT_REWRAP_WRAPS_STAGED &&
             o->failure_from_state <= DB2_VAULT_REWRAP_PROMOTED && staged;
   }
   return 0;
}

int db2_vault_operator_rewrap_bind(db2_vault_operator_runtime_t *r)
{
   if (!r || !r->connection || !r->mutex_initialized)
      return -1;
   pthread_mutex_lock(&binding_mutex);
   int rc = bound_runtime && bound_runtime != r ? -1 : 0;
   if (!rc)
   {
      bound_runtime = r;
      uncertain_connection = NULL;
   }
   pthread_mutex_unlock(&binding_mutex);
   return rc;
}
void db2_vault_operator_rewrap_unbind(db2_vault_operator_runtime_t *r)
{
   pthread_mutex_lock(&binding_mutex);
   if (bound_runtime == r)
   {
      if (r && uncertain_connection == r->connection)
         uncertain_connection = NULL;
      bound_runtime = NULL;
   }
   pthread_mutex_unlock(&binding_mutex);
}
static db2_vault_operator_runtime_t *binding(void)
{
   pthread_mutex_lock(&binding_mutex);
   db2_vault_operator_runtime_t *r = bound_runtime;
   pthread_mutex_unlock(&binding_mutex);
   return r;
}

static int authority_boolean(PGconn *connection, const char *sql)
{
   PGresult *result = NULL;
   int valid = 0;
   db2_vault_rewrap_result_t rc = query(connection, sql, 0, NULL, NULL, NULL, NULL, &result);
   if (rc == DB2_VAULT_REWRAP_OK && PQntuples(result) == 1 && PQnfields(result) == 1 &&
       !PQgetisnull(result, 0, 0) && PQgetlength(result, 0, 0) == 1)
      valid = *(const unsigned char *)PQgetvalue(result, 0, 0) == 1;
   PQclear(result);
   return valid ? 0 : -1;
}

static int recover_uncertain_locked(db2_vault_operator_runtime_t *runtime)
{
   static const char before[] =
       "SELECT session_user='aimee_kb_vault_orchestrator_login' AND "
       "current_user=session_user AND l.rolcanlogin AND NOT l.rolinherit AND NOT l.rolsuper AND "
       "NOT l.rolbypassrls AND NOT l.rolcreatedb AND NOT l.rolcreaterole AND NOT "
       "l.rolreplication AND pg_catalog.pg_has_role(session_user,"
       "'aimee_kb_vault_orchestrator','MEMBER') AND (SELECT count(*) FROM "
       "pg_catalog.pg_auth_members m WHERE m.member=l.oid)=1 AND NOT EXISTS (SELECT 1 FROM "
       "pg_catalog.pg_auth_members m JOIN pg_catalog.pg_roles granted ON granted.oid=m.roleid "
       "WHERE m.member=l.oid AND granted.rolname<>'aimee_kb_vault_orchestrator') FROM "
       "pg_catalog.pg_roles l WHERE "
       "l.rolname=session_user";
   static const char after[] =
       "SELECT session_user='aimee_kb_vault_orchestrator_login' AND "
       "current_user='aimee_kb_vault_orchestrator' AND NOT o.rolcanlogin AND NOT o.rolinherit "
       "AND NOT o.rolsuper AND NOT o.rolbypassrls AND NOT o.rolcreatedb AND NOT "
       "o.rolcreaterole AND NOT o.rolreplication AND "
       "pg_catalog.current_setting('search_path')='pg_catalog, pg_temp' AND "
       "pg_catalog.current_setting('row_security')='on' AND "
       "pg_catalog.current_setting('statement_timeout')='1900ms' AND "
       "pg_catalog.current_setting('lock_timeout')='1900ms' AND NOT "
       "pg_catalog.has_schema_privilege(current_user,'public','USAGE') AND "
       "pg_catalog.has_schema_privilege(current_user,'aimee_kb_vault_orchestrator_api','USAGE') "
       "AND NOT pg_catalog.has_schema_privilege(current_user,"
       "'aimee_kb_vault_orchestrator_api','CREATE') AND (SELECT pg_catalog.count(*)=27 AND "
       "pg_catalog.count(*) FILTER (WHERE "
       "(p.oid::pg_catalog.regprocedure)::TEXT=ANY(" ORCHESTRATOR_FUNCTIONS_SQL
       ") AND pg_catalog.has_function_privilege(current_user,p.oid,'EXECUTE'))=27 FROM "
       "pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE "
       "n.nspname='aimee_kb_vault_orchestrator_api') AND NOT EXISTS (SELECT "
       "1 FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE "
       "pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname NOT IN "
       "('information_schema','aimee_kb_vault_orchestrator_api') AND "
       "pg_catalog.has_schema_privilege(current_user,n.oid,'USAGE') AND "
       "pg_catalog.has_function_privilege(current_user,p.oid,'EXECUTE')) AND NOT EXISTS (SELECT "
       "1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
       "WHERE pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema' AND "
       "CASE WHEN c.relkind IN ('r','p','v','m','f') THEN "
       "pg_catalog.has_table_privilege(current_user,c.oid,"
       "'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') ELSE false END) FROM "
       "pg_catalog.pg_roles o WHERE "
       "o.rolname=current_user";
   PGconn *connection = runtime ? (PGconn *)runtime->connection : NULL;
   if (!connection || !connection_uncertain(connection))
      return 0;
   int64_t end = deadline();
   if (end < 0 || !PQresetStart(connection))
      goto fail;
   for (;;)
   {
      PostgresPollingStatusType state = PQresetPoll(connection);
      if (state == PGRES_POLLING_OK)
         break;
      if (state == PGRES_POLLING_ACTIVE)
         continue;
      if (state == PGRES_POLLING_FAILED ||
          wait_socket(connection, state == PGRES_POLLING_READING ? POLLIN : POLLOUT, end) != 0)
         goto fail;
   }
   pthread_mutex_lock(&binding_mutex);
   if (bound_runtime != runtime || uncertain_connection != connection)
   {
      pthread_mutex_unlock(&binding_mutex);
      goto fail;
   }
   uncertain_connection = NULL;
   pthread_mutex_unlock(&binding_mutex);
   if (command(connection, "SET search_path = pg_catalog, pg_temp") ||
       command(connection, "SET row_security = on") ||
       command(connection, "SET statement_timeout = '1900ms'") ||
       command(connection, "SET lock_timeout = '1900ms'") ||
       authority_boolean(connection, before) ||
       command(connection, "SET ROLE aimee_kb_vault_orchestrator") ||
       authority_boolean(connection, after))
      goto fail;
   runtime->transaction_active = 0;
   return 1;
fail:
   mark_uncertain(connection);
   return -1;
}

int db2_vault_operator_rewrap_recover_uncertain(void)
{
   db2_vault_operator_runtime_t *runtime = binding();
   int64_t end = deadline();
   if (!runtime || end < 0 || lock_until(runtime_mutex(runtime), end))
      return -1;
   int rc = recover_uncertain_locked(runtime);
   pthread_mutex_unlock(runtime_mutex(runtime));
   return rc;
}

static db2_vault_rewrap_result_t tx_begin(db2_vault_rewrap_tx_t **out)
{
   if (!out || *out)
      return DB2_VAULT_REWRAP_INVALID;
   db2_vault_operator_runtime_t *r = binding();
   int64_t end = deadline();
   if (!r || end < 0 || lock_until(runtime_mutex(r), end))
      return DB2_VAULT_REWRAP_TRANSIENT;
   if (r->transaction_active ||
       command((PGconn *)r->connection, "BEGIN ISOLATION LEVEL SERIALIZABLE") !=
           DB2_VAULT_REWRAP_OK)
   {
      pthread_mutex_unlock(runtime_mutex(r));
      return DB2_VAULT_REWRAP_TRANSIENT;
   }
   db2_vault_rewrap_tx_t *t = calloc(1, sizeof(*t));
   if (!t)
   {
      (void)command((PGconn *)r->connection, "ROLLBACK");
      pthread_mutex_unlock(runtime_mutex(r));
      return DB2_VAULT_REWRAP_ERROR;
   }
   r->transaction_active = 1;
   t->runtime = r;
   t->owner = pthread_self();
   t->phase = TX_GENERAL;
   *out = t;
   return DB2_VAULT_REWRAP_OK;
}
static int tx_valid(db2_vault_rewrap_tx_t *t)
{
   return t && t->runtime && pthread_equal(t->owner, pthread_self()) && t->phase != TX_FAILED;
}
static db2_vault_rewrap_result_t fail(db2_vault_rewrap_tx_t *t, db2_vault_rewrap_result_t rc)
{
   if (t)
      t->phase = TX_FAILED;
   return rc;
}
static db2_vault_rewrap_result_t tx_end(db2_vault_rewrap_tx_t **tp, int commit)
{
   if (!tp || !tx_valid(*tp))
      return DB2_VAULT_REWRAP_INVALID;
   db2_vault_rewrap_tx_t *t = *tp;
   if (commit && t->phase != TX_SINGLE_DONE && t->phase != TX_STAGE_DONE &&
       t->phase != TX_PROMOTE_DONE && t->phase != TX_COMPLETE)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_result_t rc =
       command((PGconn *)t->runtime->connection, commit ? "COMMIT" : "ROLLBACK");
   if (commit && rc != DB2_VAULT_REWRAP_OK)
      rc = DB2_VAULT_REWRAP_TRANSIENT;
   db2_vault_operator_runtime_t *r = t->runtime;
   r->transaction_active = 0;
   /* COMMIT can have taken effect even when its result was lost.  Restore a
    * separately authenticated session, but preserve TRANSIENT so D2 performs
    * its normal fresh-snapshot replay classification. */
   if (connection_uncertain((PGconn *)r->connection))
      (void)recover_uncertain_locked(r);
   OPENSSL_cleanse(t, sizeof(*t));
   free(t);
   *tp = NULL;
   pthread_mutex_unlock(runtime_mutex(r));
   return rc;
}
static db2_vault_rewrap_result_t tx_commit(db2_vault_rewrap_tx_t **t)
{
   return tx_end(t, 1);
}
static void tx_rollback(db2_vault_rewrap_tx_t **t)
{
   if (t && *t)
      (void)tx_end(t, 0);
}

static int snapshot_decode(PGresult *r, db2_vault_rewrap_snapshot_t *o)
{
   char op[33];
   if (PQntuples(r) != 1 || PQnfields(r) != 14 || col_text(r, 0, 0, op, sizeof(op)) ||
       db2_vault_reseal_operation_id_from_hex(op, o->operation_id) ||
       state_parse(r, 0, 1, &o->state) || col_i64(r, 0, 2, &o->seal_epoch) ||
       col_i64(r, 0, 3, &o->fencing_token) || col_i64(r, 0, 4, &o->old_generation) ||
       col_i64(r, 0, 5, &o->new_generation) || col_i64(r, 0, 8, &o->secret_count) ||
       col_i64(r, 0, 9, &o->check_count))
      return -1;
   if (o->seal_epoch < 1 || o->fencing_token < 1 || o->old_generation < 0 ||
       o->old_generation == INT64_MAX || o->new_generation != o->old_generation + 1 ||
       o->secret_count < 0 || o->check_count < 0)
      return -1;
   if (!PQgetisnull(r, 0, 6))
   {
      if (col_blob(r, 0, 6, o->receipt, sizeof(o->receipt)) ||
          col_blob(r, 0, 7, o->receipt_digest, 32))
         return -1;
      vault_tpm2_reseal_receipt_t rr;
      uint8_t digest[32];
      if (db2_vault_reseal_receipt_decode(o->receipt, sizeof(o->receipt), &rr) ||
          db2_vault_reseal_receipt_digest(o->receipt, digest) ||
          CRYPTO_memcmp(digest, o->receipt_digest, 32) ||
          CRYPTO_memcmp(rr.operation_id, o->operation_id, 16) ||
          rr.old_generation != (uint64_t)o->old_generation ||
          rr.new_generation != (uint64_t)o->new_generation)
      {
         OPENSSL_cleanse(&rr, sizeof(rr));
         OPENSSL_cleanse(digest, sizeof(digest));
         return -1;
      }
      OPENSSL_cleanse(&rr, sizeof(rr));
      OPENSSL_cleanse(digest, sizeof(digest));
      o->has_receipt = 1;
   }
   else if (!PQgetisnull(r, 0, 7))
      return -1;
   if (!PQgetisnull(r, 0, 10))
   {
      if (col_blob(r, 0, 10, o->inventory_digest, 32))
         return -1;
      o->has_inventory = 1;
   }
   if (!PQgetisnull(r, 0, 11))
   {
      if (col_blob(r, 0, 11, o->stage_digest, 32))
         return -1;
      o->has_stage = 1;
   }
   if (!PQgetisnull(r, 0, 12) && col_text(r, 0, 12, o->failure_class, sizeof(o->failure_class)))
      return -1;
   if (!PQgetisnull(r, 0, 13))
   {
      if (state_parse(r, 0, 13, &o->failure_from_state))
         return -1;
      o->has_failure_from_state = 1;
   }
   return o->has_inventory == o->has_stage && failure_valid(o->failure_class) && snapshot_shape(o)
              ? 0
              : -1;
}
static db2_vault_rewrap_result_t snapshot(const uint8_t op[16], db2_vault_rewrap_snapshot_t *out)
{
   if (out)
      db2_vault_rewrap_snapshot_clear(out);
   if (!op || !out)
      return DB2_VAULT_REWRAP_INVALID;
   db2_vault_operator_runtime_t *rt = binding();
   int64_t end = deadline();
   char id[33];
   if (!rt || end < 0 || op_hex(op, id) || lock_until(runtime_mutex(rt), end))
      return DB2_VAULT_REWRAP_TRANSIENT;
   Oid ty[] = {25};
   const char *v[] = {id};
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc =
       query(rt->connection, "SELECT * FROM " API "org_vault_rewrap_snapshot($1)", 1, ty, v, NULL,
             NULL, &r);
   if (rc == DB2_VAULT_REWRAP_OK)
   {
      if (PQntuples(r) == 0)
         rc = DB2_VAULT_REWRAP_NOT_FOUND;
      else if (snapshot_decode(r, out) || CRYPTO_memcmp(op, out->operation_id, 16))
         rc = DB2_VAULT_REWRAP_INTEGRITY;
   }
   PQclear(r);
   pthread_mutex_unlock(runtime_mutex(rt));
   if (rc != DB2_VAULT_REWRAP_OK)
      db2_vault_rewrap_snapshot_clear(out);
   return rc;
}

static db2_vault_rewrap_result_t state_result(PGresult *r, db2_vault_rewrap_state_t *out)
{
   if (PQntuples(r) != 1 || PQnfields(r) != 1 || state_parse(r, 0, 0, out))
      return DB2_VAULT_REWRAP_INTEGRITY;
   return DB2_VAULT_REWRAP_OK;
}
static db2_vault_rewrap_result_t begin(db2_vault_rewrap_tx_t *t, const char *actor,
                                       const char *request, const uint8_t op[16], int64_t oldg,
                                       int64_t newg, int64_t *epoch, int64_t *fence,
                                       db2_vault_rewrap_state_t *state)
{
   if (!tx_valid(t) || t->phase != TX_GENERAL || !actor || !request || !op || !epoch || !fence ||
       !state || oldg < 0 || oldg == INT64_MAX || newg != oldg + 1)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char id[33], ob[32], nb[32];
   if (op_hex(op, id))
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   snprintf(ob, sizeof(ob), "%lld", (long long)oldg);
   snprintf(nb, sizeof(nb), "%lld", (long long)newg);
   Oid ty[] = {25, 25, 25, 20, 20};
   const char *v[] = {actor, request, id, ob, nb};
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc = query(
       t->runtime->connection, "SELECT * FROM " API "org_vault_rewrap_reserve($1,$2,$3,$4,$5)", 5,
       ty, v, NULL, NULL, &r);
   char returned[33];
   int created;
   if (rc == DB2_VAULT_REWRAP_OK &&
       (PQntuples(r) != 1 || PQnfields(r) != 9 || col_bool(r, 0, 0, &created) ||
        col_text(r, 0, 1, returned, sizeof(returned)) || strcmp(returned, id) ||
        state_parse(r, 0, 4, state) || col_i64(r, 0, 5, epoch) || col_i64(r, 0, 6, fence) ||
        *epoch < 1 || *fence < 1))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   PQclear(r);
   if (rc == DB2_VAULT_REWRAP_OK)
      t->phase = TX_SINGLE_DONE;
   else
      fail(t, rc);
   return rc;
}

static db2_vault_rewrap_result_t op_query(db2_vault_rewrap_tx_t *t, const char *fn,
                                          const uint8_t op[16], int64_t fence, int extra,
                                          const Oid *ety, const char *const *ev, const int *el,
                                          const int *ef, db2_vault_rewrap_state_t *state)
{
   if (!tx_valid(t) || !op || fence < 1)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char id[33], fb[32];
   if (op_hex(op, id))
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   snprintf(fb, sizeof(fb), "%lld", (long long)fence);
   Oid ty[10] = {25, 20};
   const char *v[10] = {id, fb};
   int lens[10] = {0}, fmts[10] = {0};
   for (int i = 0; i < extra; i++)
   {
      ty[i + 2] = ety[i];
      v[i + 2] = ev[i];
      lens[i + 2] = el ? el[i] : 0;
      fmts[i + 2] = ef ? ef[i] : 0;
   }
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT %s%s($1,$2%s%s%s%s%s%s%s%s)", API, fn, extra > 0 ? ",$3" : "",
            extra > 1 ? ",$4" : "", extra > 2 ? ",$5" : "", extra > 3 ? ",$6" : "",
            extra > 4 ? ",$7" : "", extra > 5 ? ",$8" : "", extra > 6 ? ",$9" : "",
            extra > 7 ? ",$10" : "");
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc =
       query(t->runtime->connection, sql, extra + 2, ty, v, lens, fmts, &r);
   if (rc == DB2_VAULT_REWRAP_OK)
   {
      if (state)
         rc = state_result(r, state);
      else if (PQntuples(r) != 1 || PQnfields(r) != 1 || !PQgetisnull(r, 0, 0))
         rc = DB2_VAULT_REWRAP_INTEGRITY;
   }
   PQclear(r);
   return rc;
}
static db2_vault_rewrap_result_t single_state(db2_vault_rewrap_tx_t *t, const char *fn,
                                              const uint8_t op[16], int64_t f, int extra,
                                              const Oid *ty, const char *const *v, const int *l,
                                              const int *fmt)
{
   db2_vault_rewrap_state_t s;
   db2_vault_rewrap_result_t rc = op_query(t, fn, op, f, extra, ty, v, l, fmt, &s);
   if (rc == DB2_VAULT_REWRAP_OK)
      t->phase = TX_SINGLE_DONE;
   else
      fail(t, rc);
   return rc;
}
static db2_vault_rewrap_result_t record_prepared(db2_vault_rewrap_tx_t *t, const uint8_t op[16],
                                                 int64_t f, int64_t og, int64_t ng,
                                                 const uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN])
{
   (void)og;
   (void)ng;
   uint8_t d[32];
   if (!receipt || db2_vault_reseal_receipt_digest(receipt, d))
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   Oid ty[] = {17, 17};
   const char *v[] = {(char *)receipt, (char *)d};
   int l[] = {VAULT_RESEAL_RECEIPT_V1_LEN, 32}, fmt[] = {1, 1};
   db2_vault_rewrap_result_t rc =
       single_state(t, "org_vault_rewrap_record_prepared", op, f, 2, ty, v, l, fmt);
   OPENSSL_cleanse(d, sizeof(d));
   return rc;
}

static db2_vault_rewrap_result_t secret_page(db2_vault_rewrap_tx_t *t, const char *fn,
                                             const uint8_t op[16], int64_t f, int64_t after,
                                             int limit, db2_vault_rewrap_secret_t *rows, size_t cap,
                                             size_t *count, int verify)
{
   if (rows && cap <= DB2_VAULT_REWRAP_PAGE_MAX)
      db2_vault_rewrap_secret_clear(rows, cap);
   if (count)
      *count = 0;
   if (!tx_valid(t) || !rows || !count || cap > (size_t)DB2_VAULT_REWRAP_PAGE_MAX || limit < 1 ||
       limit > DB2_VAULT_REWRAP_PAGE_MAX || cap < (size_t)limit || after < 0)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char id[33], fb[32], ab[32], lb[16];
   op_hex(op, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   snprintf(ab, sizeof(ab), "%lld", (long long)after);
   snprintf(lb, sizeof(lb), "%d", limit);
   Oid ty[] = {25, 20, 20, 23};
   const char *v[] = {id, fb, ab, lb};
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT * FROM %s%s($1,$2,$3,$4)", API, fn);
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc = query(t->runtime->connection, sql, 4, ty, v, NULL, NULL, &r);
   if (rc == DB2_VAULT_REWRAP_OK)
   {
      int n = PQntuples(r), cols = PQnfields(r);
      if (n > limit || cols != (verify ? 6 : 7))
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      for (int i = 0; rc == DB2_VAULT_REWRAP_OK && i < n; i++)
      {
         db2_vault_rewrap_secret_t *x = &rows[i];
         if (col_i64(r, i, 0, &x->source_id) ||
             col_text(r, i, 1, x->principal, sizeof(x->principal)) ||
             col_text(r, i, 2, x->agent, sizeof(x->agent)) ||
             col_text(r, i, 3, x->cred, sizeof(x->cred)) || col_i64(r, i, 4, &x->version) ||
             x->source_id <= after || x->version < 1 ||
             (!verify && col_blob(r, i, 5, x->source_digest, 32)) ||
             col_blob(r, i, verify ? 5 : 6, x->wrapped_dek, VAULT_WRAPPED_DEK_LEN))
            rc = DB2_VAULT_REWRAP_INTEGRITY;
         else
         {
            after = x->source_id;
            (*count)++;
         }
      }
   }
   PQclear(r);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      db2_vault_rewrap_secret_clear(rows, cap);
      *count = 0;
      fail(t, rc);
   }
   return rc;
}
static db2_vault_rewrap_result_t source_secret_page(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                    int64_t f, int64_t a, int l,
                                                    db2_vault_rewrap_secret_t *r, size_t c,
                                                    size_t *n)
{
   if (t && t->phase == TX_GENERAL)
      t->phase = TX_STAGING;
   if (!t || t->phase != TX_STAGING || a != t->last_secret || t->secret_exhausted)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_result_t rc =
       secret_page(t, "org_vault_rewrap_secret_page", o, f, a, l, r, c, n, 0);
   if (!rc)
   {
      if (*n)
         t->last_secret = r[*n - 1].source_id;
      else
         t->secret_exhausted = 1;
   }
   return rc;
}

static int utf8_valid(const uint8_t *s, size_t n)
{
   size_t i = 0;
   while (i < n)
   {
      uint8_t c = s[i++];
      if (!c)
         return 0;
      if (c < 128)
         continue;
      unsigned q;
      uint32_t cp;
      if (c >= 0xc2 && c <= 0xdf)
         q = 1, cp = c & 31;
      else if (c >= 0xe0 && c <= 0xef)
         q = 2, cp = c & 15;
      else if (c >= 0xf0 && c <= 0xf4)
         q = 3, cp = c & 7;
      else
         return 0;
      if (n - i < q)
         return 0;
      for (unsigned j = 0; j < q; j++)
      {
         uint8_t d = s[i++];
         if ((d & 0xc0) != 0x80)
            return 0;
         cp = (cp << 6) | (d & 63);
      }
      if ((q == 2 && cp < 0x800) || (q == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) ||
          cp > 0x10ffff)
         return 0;
   }
   return 1;
}
static int cursor_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
   size_t n = an < bn ? an : bn;
   int c = n ? memcmp(a, b, n) : 0;
   return c ? c : (an > bn) - (an < bn);
}
static db2_vault_rewrap_result_t
check_page(db2_vault_rewrap_tx_t *t, const char *fn, const uint8_t o[16], int64_t f,
           const db2_vault_rewrap_cursor_t *a, int lim, db2_vault_rewrap_check_t *rows, size_t cap,
           size_t *count, db2_vault_rewrap_cursor_t *next, int verify)
{
   static const uint8_t empty = 0;
   if (rows && cap <= DB2_VAULT_REWRAP_PAGE_MAX)
      db2_vault_rewrap_check_clear(rows, cap);
   if (count)
      *count = 0;
   if (next)
      db2_vault_rewrap_cursor_clear(next);
   if (!tx_valid(t) || !a || a->len > 640 || !rows || !count || !next || lim < 1 || lim > 128 ||
       cap < (size_t)lim || cap > 128)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char id[33], fb[32], lb[16];
   op_hex(o, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   snprintf(lb, sizeof(lb), "%d", lim);
   Oid ty[] = {25, 20, 17, 23};
   const char *v[] = {id, fb, (char *)(a->len ? a->bytes : &empty), lb};
   int lens[] = {0, 0, (int)a->len, 0}, fmts[] = {0, 0, 1, 0};
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT * FROM %s%s($1,$2,$3,$4)", API, fn);
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc = query(t->runtime->connection, sql, 4, ty, v, lens, fmts, &r);
   *next = *a;
   if (rc == DB2_VAULT_REWRAP_OK)
   {
      int n = PQntuples(r);
      if (n > lim || PQnfields(r) != (verify ? 3 : 4))
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      for (int i = 0; rc == DB2_VAULT_REWRAP_OK && i < n; i++)
      {
         db2_vault_rewrap_check_t *x = &rows[i];
         if (col_text(r, i, 0, x->principal, sizeof(x->principal)))
            rc = DB2_VAULT_REWRAP_INTEGRITY;
         int kc = verify ? 1 : 2, cc = verify ? 2 : 3, kn = PQgetlength(r, i, kc),
             cn = PQgetlength(r, i, cc);
         uint8_t *cur = (uint8_t *)PQgetvalue(r, i, cc);
         size_t pn = strlen(x->principal);
         if (PQgetisnull(r, i, kc) || PQgetisnull(r, i, cc) ||
             (kn != 0 && kn != VAULT_WRAPPED_DEK_LEN) || cn < 1 || cn > 640 || pn != (size_t)cn ||
             memcmp(x->principal, cur, pn) || !utf8_valid(cur, cn) ||
             cursor_cmp(cur, cn, next->bytes, next->len) <= 0)
            rc = DB2_VAULT_REWRAP_INTEGRITY;
         else
         {
            if (kn)
               memcpy(x->kek_check, PQgetvalue(r, i, kc), kn);
            x->kek_check_len = kn;
            if (!verify && col_blob(r, i, 1, x->source_digest, 32))
               rc = DB2_VAULT_REWRAP_INTEGRITY;
            else
            {
               memcpy(next->bytes, cur, cn);
               next->len = cn;
               (*count)++;
            }
         }
      }
   }
   PQclear(r);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      db2_vault_rewrap_check_clear(rows, cap);
      *count = 0;
      db2_vault_rewrap_cursor_clear(next);
      fail(t, rc);
   }
   return rc;
}
static db2_vault_rewrap_result_t source_check_page(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                   int64_t f, const db2_vault_rewrap_cursor_t *a,
                                                   int l, db2_vault_rewrap_check_t *r, size_t c,
                                                   size_t *n, db2_vault_rewrap_cursor_t *x)
{
   if (t && t->phase == TX_GENERAL)
      t->phase = TX_STAGING;
   if (!t || t->phase != TX_STAGING || !a || a->len != t->cursor.len ||
       CRYPTO_memcmp(a->bytes, t->cursor.bytes, a->len) || t->check_exhausted)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_result_t rc =
       check_page(t, "org_vault_rewrap_check_page", o, f, a, l, r, c, n, x, 0);
   if (!rc)
   {
      t->cursor = *x;
      if (!*n)
         t->check_exhausted = 1;
   }
   return rc;
}

static db2_vault_rewrap_result_t stage_dek(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                           const db2_vault_rewrap_secret_t *s,
                                           const uint8_t nw[VAULT_WRAPPED_DEK_LEN])
{
   if (!t || t->phase != TX_STAGING || !s || !nw)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char sb[32], vb[32];
   snprintf(sb, sizeof(sb), "%lld", (long long)s->source_id);
   snprintf(vb, sizeof(vb), "%lld", (long long)s->version);
   Oid ty[] = {20, 25, 25, 25, 20, 17, 17};
   const char *v[] = {sb,        s->principal, s->agent, s->cred, vb, (char *)s->source_digest,
                      (char *)nw};
   int l[] = {0, 0, 0, 0, 0, 32, 40}, fmt[] = {0, 0, 0, 0, 0, 1, 1};
   db2_vault_rewrap_result_t rc =
       op_query(t, "org_vault_rewrap_stage_dek", o, f, 7, ty, v, l, fmt, NULL);
   if (rc != DB2_VAULT_REWRAP_OK)
      fail(t, rc);
   return rc;
}
static db2_vault_rewrap_result_t stage_check(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                             int64_t f, const db2_vault_rewrap_check_t *s,
                                             const uint8_t *nw, size_t n)
{
   static const uint8_t empty = 0;
   if (!t || t->phase != TX_STAGING || !s || (!nw && n) || (n != 0 && n != 40))
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   Oid ty[] = {25, 17, 17};
   const char *v[] = {s->principal, (char *)s->source_digest, (char *)(n ? nw : &empty)};
   int l[] = {0, 32, (int)n}, fmt[] = {0, 1, 1};
   db2_vault_rewrap_result_t rc =
       op_query(t, "org_vault_rewrap_stage_check", o, f, 3, ty, v, l, fmt, NULL);
   if (rc != 0)
      fail(t, rc);
   return rc;
}
static db2_vault_rewrap_result_t inventory_summary(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                   int64_t f,
                                                   db2_vault_rewrap_inventory_summary_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!t || (t->phase != TX_GENERAL && t->phase != TX_STAGING) || !out)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char id[33], fb[32];
   op_hex(o, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   Oid ty[] = {25, 20};
   const char *v[] = {id, fb};
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc = query(
       t->runtime->connection, "SELECT * FROM " API "org_vault_rewrap_inventory_summary($1,$2)", 2,
       ty, v, NULL, NULL, &r);
   if (rc == DB2_VAULT_REWRAP_OK &&
       (PQntuples(r) != 1 || PQnfields(r) != 3 || col_i64(r, 0, 0, &out->secret_count) ||
        col_i64(r, 0, 1, &out->check_count) || out->secret_count < 0 || out->check_count < 0 ||
        col_blob(r, 0, 2, out->inventory_digest, 32)))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   PQclear(r);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      memset(out, 0, sizeof(*out));
      return fail(t, rc);
   }
   t->phase = TX_STAGING;
   return rc;
}

static db2_vault_rewrap_result_t stage_finish(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                              int64_t f,
                                              const db2_vault_rewrap_inventory_summary_t *expected)
{
   if (!t || !t->secret_exhausted || !t->check_exhausted || !expected ||
       expected->secret_count < 0 || expected->check_count < 0)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char sb[32], cb[32];
   snprintf(sb, sizeof(sb), "%lld", (long long)expected->secret_count);
   snprintf(cb, sizeof(cb), "%lld", (long long)expected->check_count);
   Oid ty[] = {20, 20, 17};
   const char *v[] = {sb, cb, (const char *)expected->inventory_digest};
   int l[] = {0, 0, 32}, fmt[] = {0, 0, 1};
   db2_vault_rewrap_result_t rc =
       single_state(t, "org_vault_rewrap_stage_finish", o, f, 3, ty, v, l, fmt);
   if (!rc)
      t->phase = TX_STAGE_DONE;
   return rc;
}
static db2_vault_rewrap_result_t mark_committing(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                 int64_t f)
{
   return single_state(t, "org_vault_rewrap_mark_committing", o, f, 0, NULL, NULL, NULL, NULL);
}
static db2_vault_rewrap_result_t mark_resealed(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                               int64_t f, const uint8_t d[32])
{
   Oid ty[] = {17};
   const char *v[] = {(char *)d};
   int l[] = {32}, fmt[] = {1};
   return d ? single_state(t, "org_vault_rewrap_mark_resealed", o, f, 1, ty, v, l, fmt)
            : fail(t, DB2_VAULT_REWRAP_INVALID);
}
static db2_vault_rewrap_result_t promote(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f)
{
   db2_vault_rewrap_result_t rc =
       single_state(t, "org_vault_rewrap_promote", o, f, 0, NULL, NULL, NULL, NULL);
   if (!rc)
      t->phase = TX_PROMOTE_DONE;
   return rc;
}
static db2_vault_rewrap_result_t abort_op(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                          const char *x)
{
   Oid ty[] = {25};
   const char *v[] = {x};
   return x && *x ? single_state(t, "org_vault_rewrap_abort", o, f, 1, ty, v, NULL, NULL)
                  : fail(t, DB2_VAULT_REWRAP_INVALID);
}
static db2_vault_rewrap_result_t recovery(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                          const char *x)
{
   Oid ty[] = {25};
   const char *v[] = {x};
   return x && *x
              ? single_state(t, "org_vault_rewrap_recovery_required", o, f, 1, ty, v, NULL, NULL)
              : fail(t, DB2_VAULT_REWRAP_INVALID);
}

static db2_vault_rewrap_result_t verify_summary(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                int64_t f, db2_vault_rewrap_verify_summary_t *out)
{
   if (out)
      db2_vault_rewrap_verify_summary_clear(out);
   if (!tx_valid(t) || t->phase != TX_GENERAL || !o || !out)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   char id[33], fb[32];
   op_hex(o, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   Oid ty[] = {25, 20};
   const char *v[] = {id, fb};
   PGresult *r = NULL;
   db2_vault_rewrap_result_t rc =
       query(t->runtime->connection, "SELECT * FROM " API "org_vault_rewrap_verify_summary($1,$2)",
             2, ty, v, NULL, NULL, &r);
   if (rc == 0 &&
       (PQntuples(r) != 1 || PQnfields(r) != 5 || col_i64(r, 0, 0, &out->secret_count) ||
        col_i64(r, 0, 1, &out->check_count) || out->secret_count < 0 || out->check_count < 0 ||
        col_blob(r, 0, 2, out->receipt_digest, 32) ||
        col_blob(r, 0, 3, out->inventory_digest, 32) || col_blob(r, 0, 4, out->stage_digest, 32)))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   PQclear(r);
   if (rc)
   {
      db2_vault_rewrap_verify_summary_clear(out);
      return fail(t, rc);
   }
   memcpy(t->operation_id, o, 16);
   t->fence = f;
   t->expected_secrets = out->secret_count;
   t->expected_checks = out->check_count;
   memcpy(t->receipt_digest, out->receipt_digest, 32);
   memcpy(t->inventory_digest, out->inventory_digest, 32);
   memcpy(t->stage_digest, out->stage_digest, 32);
   t->phase = TX_VERIFY_SECRET;
   return rc;
}
static db2_vault_rewrap_result_t verify_secret_page(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                    int64_t f, int64_t a, int l,
                                                    db2_vault_rewrap_secret_t *r, size_t c,
                                                    size_t *n)
{
   if (!t || t->phase != TX_VERIFY_SECRET || a != t->last_secret)
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_result_t rc =
       secret_page(t, "org_vault_rewrap_verify_secret_page", o, f, a, l, r, c, n, 1);
   if (!rc)
   {
      if ((int64_t)*n > t->expected_secrets - t->consumed_secrets)
         return fail(t, DB2_VAULT_REWRAP_INTEGRITY);
      t->consumed_secrets += *n;
      if (*n)
         t->last_secret = r[*n - 1].source_id;
      else if (t->consumed_secrets == t->expected_secrets)
         t->phase = TX_VERIFY_CHECK;
      else
         return fail(t, DB2_VAULT_REWRAP_INTEGRITY);
   }
   return rc;
}
static db2_vault_rewrap_result_t verify_check_page(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                   int64_t f, const db2_vault_rewrap_cursor_t *a,
                                                   int l, db2_vault_rewrap_check_t *r, size_t c,
                                                   size_t *n, db2_vault_rewrap_cursor_t *x)
{
   if (!t || t->phase != TX_VERIFY_CHECK || !a || a->len != t->cursor.len ||
       CRYPTO_memcmp(a->bytes, t->cursor.bytes, a->len))
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_result_t rc =
       check_page(t, "org_vault_rewrap_verify_check_page", o, f, a, l, r, c, n, x, 1);
   if (!rc)
   {
      if ((int64_t)*n > t->expected_checks - t->consumed_checks)
         return fail(t, DB2_VAULT_REWRAP_INTEGRITY);
      t->consumed_checks += *n;
      t->cursor = *x;
      if (!*n)
      {
         if (t->consumed_checks != t->expected_checks)
            return fail(t, DB2_VAULT_REWRAP_INTEGRITY);
         t->phase = TX_VERIFY_CONSUMED;
      }
   }
   return rc;
}
static db2_vault_rewrap_result_t verify_ack(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                            int64_t f)
{
   if (!tx_valid(t) || t->phase != TX_VERIFY_CONSUMED || f != t->fence ||
       CRYPTO_memcmp(o, t->operation_id, 16))
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   t->phase = TX_ACKED;
   return 0;
}
static db2_vault_rewrap_result_t complete(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                          const uint8_t r[32], const uint8_t i[32],
                                          const uint8_t s[32])
{
   if (!tx_valid(t) || t->phase != TX_ACKED || f != t->fence ||
       CRYPTO_memcmp(o, t->operation_id, 16) || CRYPTO_memcmp(r, t->receipt_digest, 32) ||
       CRYPTO_memcmp(i, t->inventory_digest, 32) || CRYPTO_memcmp(s, t->stage_digest, 32))
      return fail(t, DB2_VAULT_REWRAP_INVALID);
   Oid ty[] = {17, 17, 17};
   const char *v[] = {(char *)r, (char *)i, (char *)s};
   int l[] = {32, 32, 32}, fmt[] = {1, 1, 1};
   db2_vault_rewrap_result_t rc =
       single_state(t, "org_vault_rewrap_complete", o, f, 3, ty, v, l, fmt);
   if (!rc)
      t->phase = TX_COMPLETE;
   return rc;
}

static void bytes_hex(const uint8_t *p, size_t n, char *out)
{
   static const char h[] = "0123456789abcdef";
   for (size_t i = 0; i < n; i++)
   {
      out[i * 2] = h[p[i] >> 4];
      out[i * 2 + 1] = h[p[i] & 15];
   }
   out[n * 2] = 0;
}
static int hex_bytes(const char *s, size_t n, uint8_t *out)
{
   for (size_t i = 0; i < n; i++)
   {
      unsigned a = s[i * 2], b = s[i * 2 + 1];
      a = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : 99;
      b = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : 99;
      if (a > 15 || b > 15)
         return -1;
      out[i] = (a << 4) | b;
   }
   return s[n * 2] ? -1 : 0;
}
static int binding_row(PGresult *r, int row, int base, db2_vault_operator_rewrap_binding_t *out)
{
   char op[33], actor[16], req[33];
   if (col_text(r, row, base, op, sizeof(op)) || col_text(r, row, base + 1, actor, sizeof(actor)) ||
       strcmp(actor, "uid:0") || col_text(r, row, base + 2, req, sizeof(req)) ||
       db2_vault_reseal_operation_id_from_hex(op, out->operation_id) ||
       hex_bytes(req, 16, out->request_id) || state_parse(r, row, base + 3, &out->state) ||
       col_i64(r, row, base + 4, &out->seal_epoch) ||
       col_i64(r, row, base + 5, &out->fencing_token) ||
       col_i64(r, row, base + 6, &out->old_generation) ||
       col_i64(r, row, base + 7, &out->new_generation) || out->seal_epoch < 1 ||
       out->fencing_token < 1 || out->old_generation < 0 || out->old_generation == INT64_MAX ||
       out->new_generation != out->old_generation + 1)
      return -1;
   return 0;
}
static int direct_query(const char *sql, int n, const Oid *ty, const char *const *v, const int *l,
                        const int *f, PGresult **out)
{
   db2_vault_operator_runtime_t *rt = binding();
   int64_t end = deadline();
   if (!rt || end < 0 || lock_until(runtime_mutex(rt), end))
      return DB2_VAULT_REWRAP_TRANSIENT;
   if (rt->transaction_active)
   {
      pthread_mutex_unlock(runtime_mutex(rt));
      return DB2_VAULT_REWRAP_BUSY;
   }
   db2_vault_rewrap_result_t rc = query(rt->connection, sql, n, ty, v, l, f, out);
   pthread_mutex_unlock(runtime_mutex(rt));
   return rc;
}
int db2_vault_operator_dispatch(const uint8_t req[16], db2_vault_operator_rewrap_binding_t *out,
                                int *found)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (found)
      *found = 0;
   if (!req || !out || !found)
      return DB2_VAULT_REWRAP_INVALID;
   char rh[33];
   bytes_hex(req, 16, rh);
   Oid ty[] = {25, 25};
   const char *v[] = {"uid:0", rh};
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_dispatch($1,$2)", 2, ty, v, NULL,
                         NULL, &r);
   if (!rc)
   {
      if (PQnfields(r) != 8 || PQntuples(r) > 1)
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else if (PQntuples(r) == 1)
      {
         if (binding_row(r, 0, 0, out) || CRYPTO_memcmp(req, out->request_id, 16))
            rc = DB2_VAULT_REWRAP_INTEGRITY;
         else
            *found = 1;
      }
   }
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int db2_vault_operator_reserve(const uint8_t req[16], const uint8_t candidate[16], int64_t oldg,
                               int64_t newg, db2_vault_operator_rewrap_binding_t *out, int *created)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (created)
      *created = 0;
   if (!req || !candidate || !out || !created || oldg < 0 || oldg == INT64_MAX || newg != oldg + 1)
      return DB2_VAULT_REWRAP_INVALID;
   char rh[33], oh[33], ob[32], nb[32];
   bytes_hex(req, 16, rh);
   op_hex(candidate, oh);
   snprintf(ob, sizeof(ob), "%lld", (long long)oldg);
   snprintf(nb, sizeof(nb), "%lld", (long long)newg);
   Oid ty[] = {25, 25, 25, 20, 20};
   const char *v[] = {"uid:0", rh, oh, ob, nb};
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_reserve($1,$2,$3,$4,$5)", 5, ty, v,
                         NULL, NULL, &r);
   if (!rc && (PQntuples(r) != 1 || PQnfields(r) != 9 || col_bool(r, 0, 0, created) ||
               binding_row(r, 0, 1, out) || CRYPTO_memcmp(req, out->request_id, 16)))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int db2_vault_operator_active(db2_vault_operator_rewrap_binding_t *out, int *found)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (found)
      *found = 0;
   if (!out || !found)
      return DB2_VAULT_REWRAP_INVALID;
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_active()", 0, NULL, NULL, NULL,
                         NULL, &r);
   if (!rc)
   {
      if (PQnfields(r) != 8 || PQntuples(r) > 1)
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else if (PQntuples(r) == 1)
      {
         if (binding_row(r, 0, 0, out))
            rc = DB2_VAULT_REWRAP_INTEGRITY;
         else
            *found = 1;
      }
   }
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int db2_vault_operator_completed(const uint8_t req[16], const uint8_t op[16],
                                 db2_vault_operator_completed_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!req || !op || !out)
      return DB2_VAULT_REWRAP_INVALID;
   char rh[33], oh[33];
   bytes_hex(req, 16, rh);
   op_hex(op, oh);
   Oid ty[] = {25, 25, 25};
   const char *v[] = {"uid:0", rh, oh};
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_completed($1,$2,$3)", 3, ty, v,
                         NULL, NULL, &r);
   if (!rc)
   {
      char rop[33], actor[16], rreq[33];
      db2_vault_operator_rewrap_binding_t *b = &out->binding;
      if (PQntuples(r) != 1 || PQnfields(r) != 13 || col_text(r, 0, 0, rop, sizeof(rop)) ||
          col_text(r, 0, 1, actor, sizeof(actor)) || strcmp(actor, "uid:0") ||
          col_text(r, 0, 2, rreq, sizeof(rreq)) ||
          db2_vault_reseal_operation_id_from_hex(rop, b->operation_id) ||
          hex_bytes(rreq, 16, b->request_id) || col_i64(r, 0, 3, &b->seal_epoch) ||
          col_i64(r, 0, 4, &b->fencing_token) || col_i64(r, 0, 5, &b->old_generation) ||
          col_i64(r, 0, 6, &b->new_generation) ||
          col_blob(r, 0, 7, out->receipt, sizeof(out->receipt)) ||
          col_blob(r, 0, 8, out->receipt_digest, 32) ||
          col_blob(r, 0, 9, out->inventory_digest, 32) ||
          col_blob(r, 0, 10, out->stage_digest, 32) || col_i64(r, 0, 11, &out->secret_count) ||
          col_i64(r, 0, 12, &out->check_count) || CRYPTO_memcmp(req, b->request_id, 16) ||
          CRYPTO_memcmp(op, b->operation_id, 16) || b->new_generation != b->old_generation + 1 ||
          out->secret_count < 0 || out->check_count < 0)
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else
         b->state = DB2_VAULT_REWRAP_COMPLETED;
   }
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int db2_vault_operator_completed_active(const uint8_t op[16], db2_vault_operator_completed_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!op || !out)
      return DB2_VAULT_REWRAP_INVALID;
   char oh[33];
   op_hex(op, oh);
   Oid ty[] = {25, 25};
   const char *v[] = {"uid:0", oh};
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_completed_active($1,$2)", 2, ty, v,
                         NULL, NULL, &r);
   if (!rc)
   {
      char rop[33], actor[16], rreq[33];
      db2_vault_operator_rewrap_binding_t *b = &out->binding;
      if (PQntuples(r) != 1 || PQnfields(r) != 13 || col_text(r, 0, 0, rop, sizeof(rop)) ||
          col_text(r, 0, 1, actor, sizeof(actor)) || strcmp(actor, "uid:0") ||
          col_text(r, 0, 2, rreq, sizeof(rreq)) ||
          db2_vault_reseal_operation_id_from_hex(rop, b->operation_id) ||
          hex_bytes(rreq, 16, b->request_id) || col_i64(r, 0, 3, &b->seal_epoch) ||
          col_i64(r, 0, 4, &b->fencing_token) || col_i64(r, 0, 5, &b->old_generation) ||
          col_i64(r, 0, 6, &b->new_generation) ||
          col_blob(r, 0, 7, out->receipt, sizeof(out->receipt)) ||
          col_blob(r, 0, 8, out->receipt_digest, 32) ||
          col_blob(r, 0, 9, out->inventory_digest, 32) ||
          col_blob(r, 0, 10, out->stage_digest, 32) || col_i64(r, 0, 11, &out->secret_count) ||
          col_i64(r, 0, 12, &out->check_count) || CRYPTO_memcmp(op, b->operation_id, 16) ||
          b->new_generation != b->old_generation + 1 || out->secret_count < 0 ||
          out->check_count < 0)
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else
         b->state = DB2_VAULT_REWRAP_COMPLETED;
   }
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int db2_vault_operator_current_check_page(const db2_vault_rewrap_cursor_t *a, int limit,
                                          db2_vault_rewrap_check_t *rows, size_t cap, size_t *count,
                                          db2_vault_rewrap_cursor_t *next, int64_t *total)
{
   static const uint8_t empty = 0;
   if (rows && cap <= 128)
      db2_vault_rewrap_check_clear(rows, cap);
   if (count)
      *count = 0;
   if (next)
      db2_vault_rewrap_cursor_clear(next);
   if (total)
      *total = 0;
   if (!a || a->len > 640 || !rows || !count || !next || !total || limit < 1 || limit > 128 ||
       cap < (size_t)limit || cap > 128)
      return DB2_VAULT_REWRAP_INVALID;
   char lb[16];
   snprintf(lb, sizeof(lb), "%d", limit);
   Oid ty[] = {17, 23};
   const char *v[] = {(char *)(a->len ? a->bytes : &empty), lb};
   int lens[] = {(int)a->len, 0}, fmts[] = {1, 0};
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_current_check_page($1,$2)", 2, ty, v, lens,
                         fmts, &r);
   *next = *a;
   if (!rc)
   {
      int n = PQntuples(r);
      if (PQnfields(r) != 4 || n > limit)
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else if (n == 1 && PQgetisnull(r, 0, 0))
      {
         int64_t tc;
         if (!PQgetisnull(r, 0, 1) || PQgetisnull(r, 0, 2) || PQgetlength(r, 0, 2) != (int)a->len ||
             CRYPTO_memcmp(PQgetvalue(r, 0, 2), a->bytes, a->len) || col_i64(r, 0, 3, &tc) ||
             tc < 0)
            rc = DB2_VAULT_REWRAP_INTEGRITY;
         else
            *total = tc;
      }
      else
         for (int i = 0; !rc && i < n; i++)
         {
            db2_vault_rewrap_check_t *x = &rows[i];
            int kn = PQgetlength(r, i, 1), cn = PQgetlength(r, i, 2);
            uint8_t *cur = (uint8_t *)PQgetvalue(r, i, 2);
            int64_t tc;
            if (PQgetisnull(r, i, 0) || col_text(r, i, 0, x->principal, sizeof(x->principal)) ||
                PQgetisnull(r, i, 1) || PQgetisnull(r, i, 2) || (kn != 0 && kn != 40) || cn < 1 ||
                cn > 640 || strlen(x->principal) != (size_t)cn || memcmp(x->principal, cur, cn) ||
                cursor_cmp(cur, cn, next->bytes, next->len) <= 0 || col_i64(r, i, 3, &tc) ||
                tc < 0 || (i && tc != *total))
               rc = DB2_VAULT_REWRAP_INTEGRITY;
            else
            {
               if (kn)
                  memcpy(x->kek_check, PQgetvalue(r, i, 1), kn);
               x->kek_check_len = kn;
               memcpy(next->bytes, cur, cn);
               next->len = cn;
               *total = tc;
               (*count)++;
            }
         }
   }
   PQclear(r);
   if (rc)
   {
      db2_vault_rewrap_check_clear(rows, cap);
      *count = 0;
      db2_vault_rewrap_cursor_clear(next);
      *total = 0;
   }
   return rc;
}
static int open_decode(PGresult *r, db2_vault_operator_open_result_t *out)
{
   char eid[65];
   return PQntuples(r) != 1 || PQnfields(r) != 4 || col_i64(r, 0, 0, &out->opened_epoch) ||
                  col_i64(r, 0, 1, &out->opened_fence) || col_text(r, 0, 2, eid, sizeof(eid)) ||
                  hex_bytes(eid, 32, out->event_id) || col_blob(r, 0, 3, out->row_hash, 32) ||
                  out->opened_epoch < 1 || out->opened_fence < 1
              ? -1
              : 0;
}
int db2_vault_operator_open_completed(const db2_vault_operator_completed_t *c,
                                      db2_vault_operator_open_result_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!c || !out || c->binding.state != DB2_VAULT_REWRAP_COMPLETED)
      return DB2_VAULT_REWRAP_INVALID;
   char rh[33], oh[33], eb[32], fb[32];
   bytes_hex(c->binding.request_id, 16, rh);
   op_hex(c->binding.operation_id, oh);
   snprintf(eb, sizeof(eb), "%lld", (long long)c->binding.seal_epoch);
   snprintf(fb, sizeof(fb), "%lld", (long long)c->binding.fencing_token);
   Oid ty[] = {25, 25, 25, 20, 20, 17, 17, 17};
   const char *v[] = {"uid:0",
                      rh,
                      oh,
                      eb,
                      fb,
                      (char *)c->receipt_digest,
                      (char *)c->inventory_digest,
                      (char *)c->stage_digest};
   int l[] = {0, 0, 0, 0, 0, 32, 32, 32}, f[] = {0, 0, 0, 0, 0, 1, 1, 1};
   PGresult *r = NULL;
   int rc =
       direct_query("SELECT * FROM " API "org_vault_rewrap_open_completed($1,$2,$3,$4,$5,$6,$7,$8)",
                    8, ty, v, l, f, &r);
   if (!rc && open_decode(r, out))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int db2_vault_operator_open_idle(const uint8_t req[16], int64_t epoch, int64_t fence,
                                 int64_t marker, db2_vault_operator_open_result_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!req || !out || epoch < 1 || fence < 1 || marker < 0 || marker > fence)
      return DB2_VAULT_REWRAP_INVALID;
   char rh[33], eb[32], fb[32], mb[32];
   bytes_hex(req, 16, rh);
   snprintf(eb, sizeof(eb), "%lld", (long long)epoch);
   snprintf(fb, sizeof(fb), "%lld", (long long)fence);
   snprintf(mb, sizeof(mb), "%lld", (long long)marker);
   Oid ty[] = {25, 25, 20, 20, 20};
   const char *v[] = {"uid:0", rh, eb, fb, mb};
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_open_idle($1,$2,$3,$4,$5)", 5, ty, v, NULL,
                         NULL, &r);
   if (!rc && open_decode(r, out))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int db2_vault_operator_open_event(const uint8_t id[32], db2_vault_operator_open_event_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!id || !out)
      return DB2_VAULT_REWRAP_INVALID;
   char eh[65];
   bytes_hex(id, 32, eh);
   Oid ty[] = {25};
   const char *v[] = {eh};
   PGresult *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_open_event($1)", 1, ty, v, NULL, NULL, &r);
   if (!rc)
   {
      char re[65], kind[32], op[33], req[33], actor[16];
      if (PQntuples(r) != 1 || PQnfields(r) != 9 || col_text(r, 0, 0, re, sizeof(re)) ||
          hex_bytes(re, 32, out->opened.event_id) || CRYPTO_memcmp(id, out->opened.event_id, 32) ||
          col_text(r, 0, 1, kind, sizeof(kind)) || col_text(r, 0, 3, req, sizeof(req)) ||
          hex_bytes(req, 16, out->request_id) || col_text(r, 0, 4, actor, sizeof(actor)) ||
          strcmp(actor, "uid:0") || col_i64(r, 0, 6, &out->opened.opened_epoch) ||
          col_i64(r, 0, 7, &out->opened.opened_fence) ||
          col_blob(r, 0, 8, out->opened.row_hash, 32))
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else if (!strcmp(kind, "completed_opened"))
      {
         out->completed_open = 1;
         out->operation_present = 1;
         if (PQgetisnull(r, 0, 2) || col_text(r, 0, 2, op, sizeof(op)) ||
             db2_vault_reseal_operation_id_from_hex(op, out->operation_id) ||
             col_i64(r, 0, 5, &out->operation_fence))
            rc = DB2_VAULT_REWRAP_INTEGRITY;
      }
      else if (strcmp(kind, "idle_opened") || !PQgetisnull(r, 0, 2) || !PQgetisnull(r, 0, 5))
         rc = DB2_VAULT_REWRAP_INTEGRITY;
   }
   PQclear(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}

const db2_vault_rewrap_ops_t db2_vault_operator_rewrap_ops = {
    .tx_begin = tx_begin,
    .tx_commit = tx_commit,
    .tx_rollback = tx_rollback,
    .snapshot = snapshot,
    .begin = begin,
    .record_prepared = record_prepared,
    .source_secret_page = source_secret_page,
    .source_check_page = source_check_page,
    .stage_dek = stage_dek,
    .stage_check = stage_check,
    .inventory_summary = inventory_summary,
    .stage_finish = stage_finish,
    .mark_committing = mark_committing,
    .mark_resealed = mark_resealed,
    .promote = promote,
    .abort = abort_op,
    .recovery_required = recovery,
    .verify_summary = verify_summary,
    .verify_secret_page = verify_secret_page,
    .verify_check_page = verify_check_page,
    .verify_crypto_ack = verify_ack,
    .complete = complete};
