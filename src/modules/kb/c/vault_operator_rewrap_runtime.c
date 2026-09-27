#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vault_operator_rewrap_runtime.h"

#include <aimee/postgres/client.h>
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

struct kb_store_vault_rewrap_tx
{
   kb_store_vault_operator_runtime_t *runtime;
   pthread_t owner;
   int phase, kind;
   uint8_t operation_id[16];
   int64_t fence, expected_secrets, expected_checks, consumed_secrets, consumed_checks;
   int64_t last_secret;
   kb_store_vault_rewrap_cursor_t cursor;
   int secret_exhausted, check_exhausted;
   uint8_t receipt_digest[32], inventory_digest[32], stage_digest[32];
};

static pthread_mutex_t binding_mutex = PTHREAD_MUTEX_INITIALIZER;
static kb_store_vault_operator_runtime_t *bound_runtime;
static aimee_postgres_session_t *uncertain_connection;

static pthread_mutex_t *runtime_mutex(kb_store_vault_operator_runtime_t *r)
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
   int64_t end = kb_store_vault_reseal_deadline_ms(D3B_DB_MS);
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

static void mark_uncertain(aimee_postgres_session_t *c)
{
   pthread_mutex_lock(&binding_mutex);
   if (bound_runtime && bound_runtime->connection == c)
      uncertain_connection = c;
   pthread_mutex_unlock(&binding_mutex);
}

static int connection_uncertain(aimee_postgres_session_t *c)
{
   pthread_mutex_lock(&binding_mutex);
   int uncertain = uncertain_connection == c;
   pthread_mutex_unlock(&binding_mutex);
   return uncertain;
}

static kb_store_vault_rewrap_result_t query(aimee_postgres_session_t *connection, const char *sql,
                                            int count, const uint32_t *types,
                                            const char *const *values, const int *lengths,
                                            const int *formats, aimee_postgres_result_t **out)
{
   *out = NULL;
   int64_t end = deadline();
   if (end < 0 || !connection || connection_uncertain(connection) || count < 0 || count > 64)
      return KB_STORE_VAULT_REWRAP_TRANSIENT;
   aimee_postgres_value_t args[64] = {0};
   for (int i = 0; i < count; i++)
   {
      if (!values || !values[i])
         continue;
      if (!formats || !formats[i])
      {
         args[i].kind = AIMEE_POSTGRES_TEXT;
         args[i].data = values[i];
         args[i].length = strlen(values[i]);
         continue;
      }
      if (!types || !lengths || lengths[i] < 0)
         return KB_STORE_VAULT_REWRAP_INTEGRITY;
      if (types[i] == 17 || types[i] == 25)
      {
         args[i].kind = types[i] == 17 ? AIMEE_POSTGRES_BYTES : AIMEE_POSTGRES_TEXT;
         args[i].data = values[i];
         args[i].length = (size_t)lengths[i];
      }
      else if ((types[i] == 20 && lengths[i] == 8) || (types[i] == 23 && lengths[i] == 4))
      {
         uint64_t number = 0;
         for (int n = 0; n < lengths[i]; n++)
            number = (number << 8) | (unsigned char)values[i][n];
         args[i].kind = AIMEE_POSTGRES_INT;
         args[i].integer = types[i] == 23 ? (int64_t)(int32_t)number : (int64_t)number;
      }
      else
         return KB_STORE_VAULT_REWRAP_INTEGRITY;
   }
   if (aimee_postgres_session_deadline(connection, end))
      return KB_STORE_VAULT_REWRAP_TRANSIENT;
   char state[6] = "", error[256] = "";
   aimee_postgres_result_t *result = aimee_postgres_session_query(
       connection, sql, args, (size_t)count, state, error, sizeof(error));
   if (!result)
   {
      if (!state[0] || !strncmp(state, "08", 2) || !strncmp(state, "57", 2))
      {
         mark_uncertain(connection);
         return KB_STORE_VAULT_REWRAP_TRANSIENT;
      }
      return kb_store_vault_rewrap_classify_sqlstate(state);
   }
   *out = result;
   return KB_STORE_VAULT_REWRAP_OK;
}
static int result_rows(const aimee_postgres_result_t *r)
{
   return (int)aimee_postgres_result_rows(r);
}
static int result_columns(const aimee_postgres_result_t *r)
{
   return (int)aimee_postgres_result_columns(r);
}
static int result_null(const aimee_postgres_result_t *r, int row, int col)
{
   return !aimee_postgres_result_cell(r, (size_t)row, (size_t)col, NULL);
}
static const void *result_value(aimee_postgres_result_t *r, int row, int col)
{
   return aimee_postgres_result_binary(r, (size_t)row, (size_t)col, NULL);
}
static int result_length(aimee_postgres_result_t *r, int row, int col)
{
   size_t length = 0;
   if (!aimee_postgres_result_binary(r, (size_t)row, (size_t)col, &length) || length > INT_MAX)
      return -1;
   return (int)length;
}

static kb_store_vault_rewrap_result_t command(aimee_postgres_session_t *c, const char *sql)
{
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc = query(c, sql, 0, NULL, NULL, NULL, NULL, &r);
   if (rc == KB_STORE_VAULT_REWRAP_OK && (result_rows(r) != 0 || result_columns(r) != 0))
      rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   aimee_postgres_result_free(r);
   return rc;
}

static uint64_t load64(const unsigned char *p)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < 8; i++)
      v = (v << 8) | p[i];
   return v;
}
static int col_i64(aimee_postgres_result_t *r, int row, int col, int64_t *out)
{
   if (result_null(r, row, col) || result_length(r, row, col) != 8)
      return -1;
   *out = (int64_t)load64((unsigned char *)result_value(r, row, col));
   return 0;
}
static int col_bool(aimee_postgres_result_t *r, int row, int col, int *out)
{
   if (result_null(r, row, col) || result_length(r, row, col) != 1)
      return -1;
   unsigned char v = *(unsigned char *)result_value(r, row, col);
   if (v > 1)
      return -1;
   *out = v;
   return 0;
}
static int col_text(aimee_postgres_result_t *r, int row, int col, char *out, size_t cap)
{
   if (result_null(r, row, col))
      return -1;
   int n = result_length(r, row, col);
   if (n < 0 || (size_t)n >= cap || memchr(result_value(r, row, col), 0, (size_t)n))
      return -1;
   memcpy(out, result_value(r, row, col), (size_t)n);
   out[n] = 0;
   return 0;
}
static int col_blob(aimee_postgres_result_t *r, int row, int col, void *out, size_t n)
{
   if (result_null(r, row, col) || result_length(r, row, col) != (int)n)
      return -1;
   if (n)
      memcpy(out, result_value(r, row, col), n);
   return 0;
}
static int state_parse(aimee_postgres_result_t *r, int row, int col,
                       kb_store_vault_rewrap_state_t *out)
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
   return op ? kb_store_vault_reseal_operation_id_to_hex(op, out) : -1;
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
static int snapshot_shape(const kb_store_vault_rewrap_snapshot_t *o)
{
   int nofail = !o->failure_class[0] && !o->has_failure_from_state;
   int none =
       !o->has_receipt && !o->has_inventory && !o->has_stage && !o->secret_count && !o->check_count;
   int prepared =
       o->has_receipt && !o->has_inventory && !o->has_stage && !o->secret_count && !o->check_count;
   int staged = o->has_receipt && o->has_inventory && o->has_stage;
   switch (o->state)
   {
   case KB_STORE_VAULT_REWRAP_PREPARING:
      return nofail && none;
   case KB_STORE_VAULT_REWRAP_CUSTODY_PREPARED:
      return nofail && prepared;
   case KB_STORE_VAULT_REWRAP_WRAPS_STAGED:
   case KB_STORE_VAULT_REWRAP_RESEAL_COMMITTING:
   case KB_STORE_VAULT_REWRAP_RESEALED:
   case KB_STORE_VAULT_REWRAP_PROMOTED:
   case KB_STORE_VAULT_REWRAP_COMPLETED:
      return nofail && staged;
   case KB_STORE_VAULT_REWRAP_ABORTED:
      return o->failure_class[0] && !o->has_failure_from_state && (none || prepared || staged);
   case KB_STORE_VAULT_REWRAP_RECOVERY_REQUIRED:
      if (!o->failure_class[0] || !o->has_failure_from_state)
         return 0;
      if (o->failure_from_state == KB_STORE_VAULT_REWRAP_PREPARING)
         return none;
      if (o->failure_from_state == KB_STORE_VAULT_REWRAP_CUSTODY_PREPARED)
         return prepared;
      return o->failure_from_state >= KB_STORE_VAULT_REWRAP_WRAPS_STAGED &&
             o->failure_from_state <= KB_STORE_VAULT_REWRAP_PROMOTED && staged;
   }
   return 0;
}

int kb_store_vault_operator_rewrap_bind(kb_store_vault_operator_runtime_t *r)
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
void kb_store_vault_operator_rewrap_unbind(kb_store_vault_operator_runtime_t *r)
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
static kb_store_vault_operator_runtime_t *binding(void)
{
   pthread_mutex_lock(&binding_mutex);
   kb_store_vault_operator_runtime_t *r = bound_runtime;
   pthread_mutex_unlock(&binding_mutex);
   return r;
}

static int authority_boolean(aimee_postgres_session_t *connection, const char *sql)
{
   aimee_postgres_result_t *result = NULL;
   int valid = 0;
   kb_store_vault_rewrap_result_t rc = query(connection, sql, 0, NULL, NULL, NULL, NULL, &result);
   if (rc == KB_STORE_VAULT_REWRAP_OK && result_rows(result) == 1 && result_columns(result) == 1 &&
       !result_null(result, 0, 0) && result_length(result, 0, 0) == 1)
      valid = *(const unsigned char *)result_value(result, 0, 0) == 1;
   aimee_postgres_result_free(result);
   return valid ? 0 : -1;
}

static int recover_uncertain_locked(kb_store_vault_operator_runtime_t *runtime)
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
   aimee_postgres_session_t *connection =
       runtime ? (aimee_postgres_session_t *)runtime->connection : NULL;
   if (!connection || !connection_uncertain(connection))
      return 0;
   int64_t end = deadline();
   char error[256] = "";
   if (end < 0 || aimee_postgres_session_deadline(connection, end) ||
       aimee_postgres_session_reconnect(connection, error, sizeof(error)))
      goto fail;
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

int kb_store_vault_operator_rewrap_recover_uncertain(void)
{
   kb_store_vault_operator_runtime_t *runtime = binding();
   int64_t end = deadline();
   if (!runtime || end < 0 || lock_until(runtime_mutex(runtime), end))
      return -1;
   int rc = recover_uncertain_locked(runtime);
   pthread_mutex_unlock(runtime_mutex(runtime));
   return rc;
}

static kb_store_vault_rewrap_result_t tx_begin(kb_store_vault_rewrap_tx_t **out)
{
   if (!out || *out)
      return KB_STORE_VAULT_REWRAP_INVALID;
   kb_store_vault_operator_runtime_t *r = binding();
   int64_t end = deadline();
   if (!r || end < 0 || lock_until(runtime_mutex(r), end))
      return KB_STORE_VAULT_REWRAP_TRANSIENT;
   if (r->transaction_active ||
       command((aimee_postgres_session_t *)r->connection, "BEGIN ISOLATION LEVEL SERIALIZABLE") !=
           KB_STORE_VAULT_REWRAP_OK)
   {
      pthread_mutex_unlock(runtime_mutex(r));
      return KB_STORE_VAULT_REWRAP_TRANSIENT;
   }
   kb_store_vault_rewrap_tx_t *t = calloc(1, sizeof(*t));
   if (!t)
   {
      (void)command((aimee_postgres_session_t *)r->connection, "ROLLBACK");
      pthread_mutex_unlock(runtime_mutex(r));
      return KB_STORE_VAULT_REWRAP_ERROR;
   }
   r->transaction_active = 1;
   t->runtime = r;
   t->owner = pthread_self();
   t->phase = TX_GENERAL;
   *out = t;
   return KB_STORE_VAULT_REWRAP_OK;
}
static int tx_valid(kb_store_vault_rewrap_tx_t *t)
{
   return t && t->runtime && pthread_equal(t->owner, pthread_self()) && t->phase != TX_FAILED;
}
static kb_store_vault_rewrap_result_t fail(kb_store_vault_rewrap_tx_t *t,
                                           kb_store_vault_rewrap_result_t rc)
{
   if (t)
      t->phase = TX_FAILED;
   return rc;
}
static kb_store_vault_rewrap_result_t tx_end(kb_store_vault_rewrap_tx_t **tp, int commit)
{
   if (!tp || !tx_valid(*tp))
      return KB_STORE_VAULT_REWRAP_INVALID;
   kb_store_vault_rewrap_tx_t *t = *tp;
   if (commit && t->phase != TX_SINGLE_DONE && t->phase != TX_STAGE_DONE &&
       t->phase != TX_PROMOTE_DONE && t->phase != TX_COMPLETE)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   kb_store_vault_rewrap_result_t rc =
       command((aimee_postgres_session_t *)t->runtime->connection, commit ? "COMMIT" : "ROLLBACK");
   if (commit && rc != KB_STORE_VAULT_REWRAP_OK)
      rc = KB_STORE_VAULT_REWRAP_TRANSIENT;
   kb_store_vault_operator_runtime_t *r = t->runtime;
   r->transaction_active = 0;
   /* COMMIT can have taken effect even when its result was lost.  Restore a
    * separately authenticated session, but preserve TRANSIENT so D2 performs
    * its normal fresh-snapshot replay classification. */
   if (connection_uncertain((aimee_postgres_session_t *)r->connection))
      (void)recover_uncertain_locked(r);
   OPENSSL_cleanse(t, sizeof(*t));
   free(t);
   *tp = NULL;
   pthread_mutex_unlock(runtime_mutex(r));
   return rc;
}
static kb_store_vault_rewrap_result_t tx_commit(kb_store_vault_rewrap_tx_t **t)
{
   return tx_end(t, 1);
}
static void tx_rollback(kb_store_vault_rewrap_tx_t **t)
{
   if (t && *t)
      (void)tx_end(t, 0);
}

static int snapshot_decode(aimee_postgres_result_t *r, kb_store_vault_rewrap_snapshot_t *o)
{
   char op[33];
   if (result_rows(r) != 1 || result_columns(r) != 14 || col_text(r, 0, 0, op, sizeof(op)) ||
       kb_store_vault_reseal_operation_id_from_hex(op, o->operation_id) ||
       state_parse(r, 0, 1, &o->state) || col_i64(r, 0, 2, &o->seal_epoch) ||
       col_i64(r, 0, 3, &o->fencing_token) || col_i64(r, 0, 4, &o->old_generation) ||
       col_i64(r, 0, 5, &o->new_generation) || col_i64(r, 0, 8, &o->secret_count) ||
       col_i64(r, 0, 9, &o->check_count))
      return -1;
   if (o->seal_epoch < 1 || o->fencing_token < 1 || o->old_generation < 0 ||
       o->old_generation == INT64_MAX || o->new_generation != o->old_generation + 1 ||
       o->secret_count < 0 || o->check_count < 0)
      return -1;
   if (!result_null(r, 0, 6))
   {
      if (col_blob(r, 0, 6, o->receipt, sizeof(o->receipt)) ||
          col_blob(r, 0, 7, o->receipt_digest, 32))
         return -1;
      vault_tpm2_reseal_receipt_t rr;
      uint8_t digest[32];
      if (kb_store_vault_reseal_receipt_decode(o->receipt, sizeof(o->receipt), &rr) ||
          kb_store_vault_reseal_receipt_digest(o->receipt, digest) ||
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
   else if (!result_null(r, 0, 7))
      return -1;
   if (!result_null(r, 0, 10))
   {
      if (col_blob(r, 0, 10, o->inventory_digest, 32))
         return -1;
      o->has_inventory = 1;
   }
   if (!result_null(r, 0, 11))
   {
      if (col_blob(r, 0, 11, o->stage_digest, 32))
         return -1;
      o->has_stage = 1;
   }
   if (!result_null(r, 0, 12) && col_text(r, 0, 12, o->failure_class, sizeof(o->failure_class)))
      return -1;
   if (!result_null(r, 0, 13))
   {
      if (state_parse(r, 0, 13, &o->failure_from_state))
         return -1;
      o->has_failure_from_state = 1;
   }
   return o->has_inventory == o->has_stage && failure_valid(o->failure_class) && snapshot_shape(o)
              ? 0
              : -1;
}
static kb_store_vault_rewrap_result_t snapshot(const uint8_t op[16],
                                               kb_store_vault_rewrap_snapshot_t *out)
{
   if (out)
      kb_store_vault_rewrap_snapshot_clear(out);
   if (!op || !out)
      return KB_STORE_VAULT_REWRAP_INVALID;
   kb_store_vault_operator_runtime_t *rt = binding();
   int64_t end = deadline();
   char id[33];
   if (!rt || end < 0 || op_hex(op, id) || lock_until(runtime_mutex(rt), end))
      return KB_STORE_VAULT_REWRAP_TRANSIENT;
   uint32_t ty[] = {25};
   const char *v[] = {id};
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc =
       query(rt->connection, "SELECT * FROM " API "org_vault_rewrap_snapshot($1)", 1, ty, v, NULL,
             NULL, &r);
   if (rc == KB_STORE_VAULT_REWRAP_OK)
   {
      if (result_rows(r) == 0)
         rc = KB_STORE_VAULT_REWRAP_NOT_FOUND;
      else if (snapshot_decode(r, out) || CRYPTO_memcmp(op, out->operation_id, 16))
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   }
   aimee_postgres_result_free(r);
   pthread_mutex_unlock(runtime_mutex(rt));
   if (rc != KB_STORE_VAULT_REWRAP_OK)
      kb_store_vault_rewrap_snapshot_clear(out);
   return rc;
}

static kb_store_vault_rewrap_result_t state_result(aimee_postgres_result_t *r,
                                                   kb_store_vault_rewrap_state_t *out)
{
   if (result_rows(r) != 1 || result_columns(r) != 1 || state_parse(r, 0, 0, out))
      return KB_STORE_VAULT_REWRAP_INTEGRITY;
   return KB_STORE_VAULT_REWRAP_OK;
}
static kb_store_vault_rewrap_result_t begin(kb_store_vault_rewrap_tx_t *t, const char *actor,
                                            const char *request, const uint8_t op[16], int64_t oldg,
                                            int64_t newg, int64_t *epoch, int64_t *fence,
                                            kb_store_vault_rewrap_state_t *state)
{
   if (!tx_valid(t) || t->phase != TX_GENERAL || !actor || !request || !op || !epoch || !fence ||
       !state || oldg < 0 || oldg == INT64_MAX || newg != oldg + 1)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char id[33], ob[32], nb[32];
   if (op_hex(op, id))
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   snprintf(ob, sizeof(ob), "%lld", (long long)oldg);
   snprintf(nb, sizeof(nb), "%lld", (long long)newg);
   uint32_t ty[] = {25, 25, 25, 20, 20};
   const char *v[] = {actor, request, id, ob, nb};
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc = query(
       t->runtime->connection, "SELECT * FROM " API "org_vault_rewrap_reserve($1,$2,$3,$4,$5)", 5,
       ty, v, NULL, NULL, &r);
   char returned[33];
   int created;
   if (rc == KB_STORE_VAULT_REWRAP_OK &&
       (result_rows(r) != 1 || result_columns(r) != 9 || col_bool(r, 0, 0, &created) ||
        col_text(r, 0, 1, returned, sizeof(returned)) || strcmp(returned, id) ||
        state_parse(r, 0, 4, state) || col_i64(r, 0, 5, epoch) || col_i64(r, 0, 6, fence) ||
        *epoch < 1 || *fence < 1))
      rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   aimee_postgres_result_free(r);
   if (rc == KB_STORE_VAULT_REWRAP_OK)
      t->phase = TX_SINGLE_DONE;
   else
      fail(t, rc);
   return rc;
}

static kb_store_vault_rewrap_result_t op_query(kb_store_vault_rewrap_tx_t *t, const char *fn,
                                               const uint8_t op[16], int64_t fence, int extra,
                                               const uint32_t *ety, const char *const *ev,
                                               const int *el, const int *ef,
                                               kb_store_vault_rewrap_state_t *state)
{
   if (!tx_valid(t) || !op || fence < 1)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char id[33], fb[32];
   if (op_hex(op, id))
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   snprintf(fb, sizeof(fb), "%lld", (long long)fence);
   uint32_t ty[10] = {25, 20};
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
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc =
       query(t->runtime->connection, sql, extra + 2, ty, v, lens, fmts, &r);
   if (rc == KB_STORE_VAULT_REWRAP_OK)
   {
      if (state)
         rc = state_result(r, state);
      else if (result_rows(r) != 1 || result_columns(r) != 1 || !result_null(r, 0, 0))
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   }
   aimee_postgres_result_free(r);
   return rc;
}
static kb_store_vault_rewrap_result_t single_state(kb_store_vault_rewrap_tx_t *t, const char *fn,
                                                   const uint8_t op[16], int64_t f, int extra,
                                                   const uint32_t *ty, const char *const *v,
                                                   const int *l, const int *fmt)
{
   kb_store_vault_rewrap_state_t s;
   kb_store_vault_rewrap_result_t rc = op_query(t, fn, op, f, extra, ty, v, l, fmt, &s);
   if (rc == KB_STORE_VAULT_REWRAP_OK)
      t->phase = TX_SINGLE_DONE;
   else
      fail(t, rc);
   return rc;
}
static kb_store_vault_rewrap_result_t
record_prepared(kb_store_vault_rewrap_tx_t *t, const uint8_t op[16], int64_t f, int64_t og,
                int64_t ng, const uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN])
{
   (void)og;
   (void)ng;
   uint8_t d[32];
   if (!receipt || kb_store_vault_reseal_receipt_digest(receipt, d))
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   uint32_t ty[] = {17, 17};
   const char *v[] = {(char *)receipt, (char *)d};
   int l[] = {VAULT_RESEAL_RECEIPT_V1_LEN, 32}, fmt[] = {1, 1};
   kb_store_vault_rewrap_result_t rc =
       single_state(t, "org_vault_rewrap_record_prepared", op, f, 2, ty, v, l, fmt);
   OPENSSL_cleanse(d, sizeof(d));
   return rc;
}

static kb_store_vault_rewrap_result_t secret_page(kb_store_vault_rewrap_tx_t *t, const char *fn,
                                                  const uint8_t op[16], int64_t f, int64_t after,
                                                  int limit, kb_store_vault_rewrap_secret_t *rows,
                                                  size_t cap, size_t *count, int verify)
{
   if (rows && cap <= KB_STORE_VAULT_REWRAP_PAGE_MAX)
      kb_store_vault_rewrap_secret_clear(rows, cap);
   if (count)
      *count = 0;
   if (!tx_valid(t) || !rows || !count || cap > (size_t)KB_STORE_VAULT_REWRAP_PAGE_MAX ||
       limit < 1 || limit > KB_STORE_VAULT_REWRAP_PAGE_MAX || cap < (size_t)limit || after < 0)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char id[33], fb[32], ab[32], lb[16];
   op_hex(op, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   snprintf(ab, sizeof(ab), "%lld", (long long)after);
   snprintf(lb, sizeof(lb), "%d", limit);
   uint32_t ty[] = {25, 20, 20, 23};
   const char *v[] = {id, fb, ab, lb};
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT * FROM %s%s($1,$2,$3,$4)", API, fn);
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc = query(t->runtime->connection, sql, 4, ty, v, NULL, NULL, &r);
   if (rc == KB_STORE_VAULT_REWRAP_OK)
   {
      int n = result_rows(r), cols = result_columns(r);
      if (n > limit || cols != (verify ? 6 : 7))
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      for (int i = 0; rc == KB_STORE_VAULT_REWRAP_OK && i < n; i++)
      {
         kb_store_vault_rewrap_secret_t *x = &rows[i];
         if (col_i64(r, i, 0, &x->source_id) ||
             col_text(r, i, 1, x->principal, sizeof(x->principal)) ||
             col_text(r, i, 2, x->agent, sizeof(x->agent)) ||
             col_text(r, i, 3, x->cred, sizeof(x->cred)) || col_i64(r, i, 4, &x->version) ||
             x->source_id <= after || x->version < 1 ||
             (!verify && col_blob(r, i, 5, x->source_digest, 32)) ||
             col_blob(r, i, verify ? 5 : 6, x->wrapped_dek, VAULT_WRAPPED_DEK_LEN))
            rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
         else
         {
            after = x->source_id;
            (*count)++;
         }
      }
   }
   aimee_postgres_result_free(r);
   if (rc != KB_STORE_VAULT_REWRAP_OK)
   {
      kb_store_vault_rewrap_secret_clear(rows, cap);
      *count = 0;
      fail(t, rc);
   }
   return rc;
}
static kb_store_vault_rewrap_result_t source_secret_page(kb_store_vault_rewrap_tx_t *t,
                                                         const uint8_t o[16], int64_t f, int64_t a,
                                                         int l, kb_store_vault_rewrap_secret_t *r,
                                                         size_t c, size_t *n)
{
   if (t && t->phase == TX_GENERAL)
      t->phase = TX_STAGING;
   if (!t || t->phase != TX_STAGING || a != t->last_secret || t->secret_exhausted)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   kb_store_vault_rewrap_result_t rc =
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
static kb_store_vault_rewrap_result_t
check_page(kb_store_vault_rewrap_tx_t *t, const char *fn, const uint8_t o[16], int64_t f,
           const kb_store_vault_rewrap_cursor_t *a, int lim, kb_store_vault_rewrap_check_t *rows,
           size_t cap, size_t *count, kb_store_vault_rewrap_cursor_t *next, int verify)
{
   static const uint8_t empty = 0;
   if (rows && cap <= KB_STORE_VAULT_REWRAP_PAGE_MAX)
      kb_store_vault_rewrap_check_clear(rows, cap);
   if (count)
      *count = 0;
   if (next)
      kb_store_vault_rewrap_cursor_clear(next);
   if (!tx_valid(t) || !a || a->len > 640 || !rows || !count || !next || lim < 1 || lim > 128 ||
       cap < (size_t)lim || cap > 128)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char id[33], fb[32], lb[16];
   op_hex(o, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   snprintf(lb, sizeof(lb), "%d", lim);
   uint32_t ty[] = {25, 20, 17, 23};
   const char *v[] = {id, fb, (char *)(a->len ? a->bytes : &empty), lb};
   int lens[] = {0, 0, (int)a->len, 0}, fmts[] = {0, 0, 1, 0};
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT * FROM %s%s($1,$2,$3,$4)", API, fn);
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc = query(t->runtime->connection, sql, 4, ty, v, lens, fmts, &r);
   *next = *a;
   if (rc == KB_STORE_VAULT_REWRAP_OK)
   {
      int n = result_rows(r);
      if (n > lim || result_columns(r) != (verify ? 3 : 4))
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      for (int i = 0; rc == KB_STORE_VAULT_REWRAP_OK && i < n; i++)
      {
         kb_store_vault_rewrap_check_t *x = &rows[i];
         if (col_text(r, i, 0, x->principal, sizeof(x->principal)))
            rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
         int kc = verify ? 1 : 2, cc = verify ? 2 : 3, kn = result_length(r, i, kc),
             cn = result_length(r, i, cc);
         uint8_t *cur = (uint8_t *)result_value(r, i, cc);
         size_t pn = strlen(x->principal);
         if (result_null(r, i, kc) || result_null(r, i, cc) ||
             (kn != 0 && kn != VAULT_WRAPPED_DEK_LEN) || cn < 1 || cn > 640 || pn != (size_t)cn ||
             memcmp(x->principal, cur, pn) || !utf8_valid(cur, cn) ||
             cursor_cmp(cur, cn, next->bytes, next->len) <= 0)
            rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
         else
         {
            if (kn)
               memcpy(x->kek_check, result_value(r, i, kc), kn);
            x->kek_check_len = kn;
            if (!verify && col_blob(r, i, 1, x->source_digest, 32))
               rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
            else
            {
               memcpy(next->bytes, cur, cn);
               next->len = cn;
               (*count)++;
            }
         }
      }
   }
   aimee_postgres_result_free(r);
   if (rc != KB_STORE_VAULT_REWRAP_OK)
   {
      kb_store_vault_rewrap_check_clear(rows, cap);
      *count = 0;
      kb_store_vault_rewrap_cursor_clear(next);
      fail(t, rc);
   }
   return rc;
}
static kb_store_vault_rewrap_result_t
source_check_page(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                  const kb_store_vault_rewrap_cursor_t *a, int l, kb_store_vault_rewrap_check_t *r,
                  size_t c, size_t *n, kb_store_vault_rewrap_cursor_t *x)
{
   if (t && t->phase == TX_GENERAL)
      t->phase = TX_STAGING;
   if (!t || t->phase != TX_STAGING || !a || a->len != t->cursor.len ||
       CRYPTO_memcmp(a->bytes, t->cursor.bytes, a->len) || t->check_exhausted)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   kb_store_vault_rewrap_result_t rc =
       check_page(t, "org_vault_rewrap_check_page", o, f, a, l, r, c, n, x, 0);
   if (!rc)
   {
      t->cursor = *x;
      if (!*n)
         t->check_exhausted = 1;
   }
   return rc;
}

static kb_store_vault_rewrap_result_t stage_dek(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                int64_t f, const kb_store_vault_rewrap_secret_t *s,
                                                const uint8_t nw[VAULT_WRAPPED_DEK_LEN])
{
   if (!t || t->phase != TX_STAGING || !s || !nw)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char sb[32], vb[32];
   snprintf(sb, sizeof(sb), "%lld", (long long)s->source_id);
   snprintf(vb, sizeof(vb), "%lld", (long long)s->version);
   uint32_t ty[] = {20, 25, 25, 25, 20, 17, 17};
   const char *v[] = {sb,        s->principal, s->agent, s->cred, vb, (char *)s->source_digest,
                      (char *)nw};
   int l[] = {0, 0, 0, 0, 0, 32, 40}, fmt[] = {0, 0, 0, 0, 0, 1, 1};
   kb_store_vault_rewrap_result_t rc =
       op_query(t, "org_vault_rewrap_stage_dek", o, f, 7, ty, v, l, fmt, NULL);
   if (rc != KB_STORE_VAULT_REWRAP_OK)
      fail(t, rc);
   return rc;
}
static kb_store_vault_rewrap_result_t stage_check(kb_store_vault_rewrap_tx_t *t,
                                                  const uint8_t o[16], int64_t f,
                                                  const kb_store_vault_rewrap_check_t *s,
                                                  const uint8_t *nw, size_t n)
{
   static const uint8_t empty = 0;
   if (!t || t->phase != TX_STAGING || !s || (!nw && n) || (n != 0 && n != 40))
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   uint32_t ty[] = {25, 17, 17};
   const char *v[] = {s->principal, (char *)s->source_digest, (char *)(n ? nw : &empty)};
   int l[] = {0, 32, (int)n}, fmt[] = {0, 1, 1};
   kb_store_vault_rewrap_result_t rc =
       op_query(t, "org_vault_rewrap_stage_check", o, f, 3, ty, v, l, fmt, NULL);
   if (rc != 0)
      fail(t, rc);
   return rc;
}
static kb_store_vault_rewrap_result_t
inventory_summary(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                  kb_store_vault_rewrap_inventory_summary_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!t || (t->phase != TX_GENERAL && t->phase != TX_STAGING) || !out)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char id[33], fb[32];
   op_hex(o, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   uint32_t ty[] = {25, 20};
   const char *v[] = {id, fb};
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc = query(
       t->runtime->connection, "SELECT * FROM " API "org_vault_rewrap_inventory_summary($1,$2)", 2,
       ty, v, NULL, NULL, &r);
   if (rc == KB_STORE_VAULT_REWRAP_OK &&
       (result_rows(r) != 1 || result_columns(r) != 3 || col_i64(r, 0, 0, &out->secret_count) ||
        col_i64(r, 0, 1, &out->check_count) || out->secret_count < 0 || out->check_count < 0 ||
        col_blob(r, 0, 2, out->inventory_digest, 32)))
      rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   aimee_postgres_result_free(r);
   if (rc != KB_STORE_VAULT_REWRAP_OK)
   {
      memset(out, 0, sizeof(*out));
      return fail(t, rc);
   }
   t->phase = TX_STAGING;
   return rc;
}

static kb_store_vault_rewrap_result_t
stage_finish(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
             const kb_store_vault_rewrap_inventory_summary_t *expected)
{
   if (!t || !t->secret_exhausted || !t->check_exhausted || !expected ||
       expected->secret_count < 0 || expected->check_count < 0)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char sb[32], cb[32];
   snprintf(sb, sizeof(sb), "%lld", (long long)expected->secret_count);
   snprintf(cb, sizeof(cb), "%lld", (long long)expected->check_count);
   uint32_t ty[] = {20, 20, 17};
   const char *v[] = {sb, cb, (const char *)expected->inventory_digest};
   int l[] = {0, 0, 32}, fmt[] = {0, 0, 1};
   kb_store_vault_rewrap_result_t rc =
       single_state(t, "org_vault_rewrap_stage_finish", o, f, 3, ty, v, l, fmt);
   if (!rc)
      t->phase = TX_STAGE_DONE;
   return rc;
}
static kb_store_vault_rewrap_result_t mark_committing(kb_store_vault_rewrap_tx_t *t,
                                                      const uint8_t o[16], int64_t f)
{
   return single_state(t, "org_vault_rewrap_mark_committing", o, f, 0, NULL, NULL, NULL, NULL);
}
static kb_store_vault_rewrap_result_t
mark_resealed(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f, const uint8_t d[32])
{
   uint32_t ty[] = {17};
   const char *v[] = {(char *)d};
   int l[] = {32}, fmt[] = {1};
   return d ? single_state(t, "org_vault_rewrap_mark_resealed", o, f, 1, ty, v, l, fmt)
            : fail(t, KB_STORE_VAULT_REWRAP_INVALID);
}
static kb_store_vault_rewrap_result_t promote(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16],
                                              int64_t f)
{
   kb_store_vault_rewrap_result_t rc =
       single_state(t, "org_vault_rewrap_promote", o, f, 0, NULL, NULL, NULL, NULL);
   if (!rc)
      t->phase = TX_PROMOTE_DONE;
   return rc;
}
static kb_store_vault_rewrap_result_t abort_op(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16],
                                               int64_t f, const char *x)
{
   uint32_t ty[] = {25};
   const char *v[] = {x};
   return x && *x ? single_state(t, "org_vault_rewrap_abort", o, f, 1, ty, v, NULL, NULL)
                  : fail(t, KB_STORE_VAULT_REWRAP_INVALID);
}
static kb_store_vault_rewrap_result_t recovery(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16],
                                               int64_t f, const char *x)
{
   uint32_t ty[] = {25};
   const char *v[] = {x};
   return x && *x
              ? single_state(t, "org_vault_rewrap_recovery_required", o, f, 1, ty, v, NULL, NULL)
              : fail(t, KB_STORE_VAULT_REWRAP_INVALID);
}

static kb_store_vault_rewrap_result_t verify_summary(kb_store_vault_rewrap_tx_t *t,
                                                     const uint8_t o[16], int64_t f,
                                                     kb_store_vault_rewrap_verify_summary_t *out)
{
   if (out)
      kb_store_vault_rewrap_verify_summary_clear(out);
   if (!tx_valid(t) || t->phase != TX_GENERAL || !o || !out)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   char id[33], fb[32];
   op_hex(o, id);
   snprintf(fb, sizeof(fb), "%lld", (long long)f);
   uint32_t ty[] = {25, 20};
   const char *v[] = {id, fb};
   aimee_postgres_result_t *r = NULL;
   kb_store_vault_rewrap_result_t rc =
       query(t->runtime->connection, "SELECT * FROM " API "org_vault_rewrap_verify_summary($1,$2)",
             2, ty, v, NULL, NULL, &r);
   if (rc == 0 &&
       (result_rows(r) != 1 || result_columns(r) != 5 || col_i64(r, 0, 0, &out->secret_count) ||
        col_i64(r, 0, 1, &out->check_count) || out->secret_count < 0 || out->check_count < 0 ||
        col_blob(r, 0, 2, out->receipt_digest, 32) ||
        col_blob(r, 0, 3, out->inventory_digest, 32) || col_blob(r, 0, 4, out->stage_digest, 32)))
      rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   aimee_postgres_result_free(r);
   if (rc)
   {
      kb_store_vault_rewrap_verify_summary_clear(out);
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
static kb_store_vault_rewrap_result_t verify_secret_page(kb_store_vault_rewrap_tx_t *t,
                                                         const uint8_t o[16], int64_t f, int64_t a,
                                                         int l, kb_store_vault_rewrap_secret_t *r,
                                                         size_t c, size_t *n)
{
   if (!t || t->phase != TX_VERIFY_SECRET || a != t->last_secret)
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   kb_store_vault_rewrap_result_t rc =
       secret_page(t, "org_vault_rewrap_verify_secret_page", o, f, a, l, r, c, n, 1);
   if (!rc)
   {
      if ((int64_t)*n > t->expected_secrets - t->consumed_secrets)
         return fail(t, KB_STORE_VAULT_REWRAP_INTEGRITY);
      t->consumed_secrets += *n;
      if (*n)
         t->last_secret = r[*n - 1].source_id;
      else if (t->consumed_secrets == t->expected_secrets)
         t->phase = TX_VERIFY_CHECK;
      else
         return fail(t, KB_STORE_VAULT_REWRAP_INTEGRITY);
   }
   return rc;
}
static kb_store_vault_rewrap_result_t
verify_check_page(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                  const kb_store_vault_rewrap_cursor_t *a, int l, kb_store_vault_rewrap_check_t *r,
                  size_t c, size_t *n, kb_store_vault_rewrap_cursor_t *x)
{
   if (!t || t->phase != TX_VERIFY_CHECK || !a || a->len != t->cursor.len ||
       CRYPTO_memcmp(a->bytes, t->cursor.bytes, a->len))
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   kb_store_vault_rewrap_result_t rc =
       check_page(t, "org_vault_rewrap_verify_check_page", o, f, a, l, r, c, n, x, 1);
   if (!rc)
   {
      if ((int64_t)*n > t->expected_checks - t->consumed_checks)
         return fail(t, KB_STORE_VAULT_REWRAP_INTEGRITY);
      t->consumed_checks += *n;
      t->cursor = *x;
      if (!*n)
      {
         if (t->consumed_checks != t->expected_checks)
            return fail(t, KB_STORE_VAULT_REWRAP_INTEGRITY);
         t->phase = TX_VERIFY_CONSUMED;
      }
   }
   return rc;
}
static kb_store_vault_rewrap_result_t verify_ack(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                 int64_t f)
{
   if (!tx_valid(t) || t->phase != TX_VERIFY_CONSUMED || f != t->fence ||
       CRYPTO_memcmp(o, t->operation_id, 16))
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   t->phase = TX_ACKED;
   return 0;
}
static kb_store_vault_rewrap_result_t complete(kb_store_vault_rewrap_tx_t *t, const uint8_t o[16],
                                               int64_t f, const uint8_t r[32], const uint8_t i[32],
                                               const uint8_t s[32])
{
   if (!tx_valid(t) || t->phase != TX_ACKED || f != t->fence ||
       CRYPTO_memcmp(o, t->operation_id, 16) || CRYPTO_memcmp(r, t->receipt_digest, 32) ||
       CRYPTO_memcmp(i, t->inventory_digest, 32) || CRYPTO_memcmp(s, t->stage_digest, 32))
      return fail(t, KB_STORE_VAULT_REWRAP_INVALID);
   uint32_t ty[] = {17, 17, 17};
   const char *v[] = {(char *)r, (char *)i, (char *)s};
   int l[] = {32, 32, 32}, fmt[] = {1, 1, 1};
   kb_store_vault_rewrap_result_t rc =
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
static int binding_row(aimee_postgres_result_t *r, int row, int base,
                       kb_store_vault_operator_rewrap_binding_t *out)
{
   char op[33], actor[16], req[33];
   if (col_text(r, row, base, op, sizeof(op)) || col_text(r, row, base + 1, actor, sizeof(actor)) ||
       strcmp(actor, "uid:0") || col_text(r, row, base + 2, req, sizeof(req)) ||
       kb_store_vault_reseal_operation_id_from_hex(op, out->operation_id) ||
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
static int direct_query(const char *sql, int n, const uint32_t *ty, const char *const *v,
                        const int *l, const int *f, aimee_postgres_result_t **out)
{
   kb_store_vault_operator_runtime_t *rt = binding();
   int64_t end = deadline();
   if (!rt || end < 0 || lock_until(runtime_mutex(rt), end))
      return KB_STORE_VAULT_REWRAP_TRANSIENT;
   if (rt->transaction_active)
   {
      pthread_mutex_unlock(runtime_mutex(rt));
      return KB_STORE_VAULT_REWRAP_BUSY;
   }
   kb_store_vault_rewrap_result_t rc = query(rt->connection, sql, n, ty, v, l, f, out);
   pthread_mutex_unlock(runtime_mutex(rt));
   return rc;
}
int kb_store_vault_operator_dispatch(const uint8_t req[16],
                                     kb_store_vault_operator_rewrap_binding_t *out, int *found)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (found)
      *found = 0;
   if (!req || !out || !found)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char rh[33];
   bytes_hex(req, 16, rh);
   uint32_t ty[] = {25, 25};
   const char *v[] = {"uid:0", rh};
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_dispatch($1,$2)", 2, ty, v, NULL,
                         NULL, &r);
   if (!rc)
   {
      if (result_columns(r) != 8 || result_rows(r) > 1)
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      else if (result_rows(r) == 1)
      {
         if (binding_row(r, 0, 0, out) || CRYPTO_memcmp(req, out->request_id, 16))
            rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
         else
            *found = 1;
      }
   }
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int kb_store_vault_operator_reserve(const uint8_t req[16], const uint8_t candidate[16],
                                    int64_t oldg, int64_t newg,
                                    kb_store_vault_operator_rewrap_binding_t *out, int *created)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (created)
      *created = 0;
   if (!req || !candidate || !out || !created || oldg < 0 || oldg == INT64_MAX || newg != oldg + 1)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char rh[33], oh[33], ob[32], nb[32];
   bytes_hex(req, 16, rh);
   op_hex(candidate, oh);
   snprintf(ob, sizeof(ob), "%lld", (long long)oldg);
   snprintf(nb, sizeof(nb), "%lld", (long long)newg);
   uint32_t ty[] = {25, 25, 25, 20, 20};
   const char *v[] = {"uid:0", rh, oh, ob, nb};
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_reserve($1,$2,$3,$4,$5)", 5, ty, v,
                         NULL, NULL, &r);
   if (!rc && (result_rows(r) != 1 || result_columns(r) != 9 || col_bool(r, 0, 0, created) ||
               binding_row(r, 0, 1, out) || CRYPTO_memcmp(req, out->request_id, 16)))
      rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int kb_store_vault_operator_active(kb_store_vault_operator_rewrap_binding_t *out, int *found)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (found)
      *found = 0;
   if (!out || !found)
      return KB_STORE_VAULT_REWRAP_INVALID;
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_active()", 0, NULL, NULL, NULL,
                         NULL, &r);
   if (!rc)
   {
      if (result_columns(r) != 8 || result_rows(r) > 1)
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      else if (result_rows(r) == 1)
      {
         if (binding_row(r, 0, 0, out))
            rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
         else
            *found = 1;
      }
   }
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int kb_store_vault_operator_completed(const uint8_t req[16], const uint8_t op[16],
                                      kb_store_vault_operator_completed_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!req || !op || !out)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char rh[33], oh[33];
   bytes_hex(req, 16, rh);
   op_hex(op, oh);
   uint32_t ty[] = {25, 25, 25};
   const char *v[] = {"uid:0", rh, oh};
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_completed($1,$2,$3)", 3, ty, v,
                         NULL, NULL, &r);
   if (!rc)
   {
      char rop[33], actor[16], rreq[33];
      kb_store_vault_operator_rewrap_binding_t *b = &out->binding;
      if (result_rows(r) != 1 || result_columns(r) != 13 || col_text(r, 0, 0, rop, sizeof(rop)) ||
          col_text(r, 0, 1, actor, sizeof(actor)) || strcmp(actor, "uid:0") ||
          col_text(r, 0, 2, rreq, sizeof(rreq)) ||
          kb_store_vault_reseal_operation_id_from_hex(rop, b->operation_id) ||
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
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      else
         b->state = KB_STORE_VAULT_REWRAP_COMPLETED;
   }
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int kb_store_vault_operator_completed_active(const uint8_t op[16],
                                             kb_store_vault_operator_completed_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!op || !out)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char oh[33];
   op_hex(op, oh);
   uint32_t ty[] = {25, 25};
   const char *v[] = {"uid:0", oh};
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_rewrap_completed_active($1,$2)", 2, ty, v,
                         NULL, NULL, &r);
   if (!rc)
   {
      char rop[33], actor[16], rreq[33];
      kb_store_vault_operator_rewrap_binding_t *b = &out->binding;
      if (result_rows(r) != 1 || result_columns(r) != 13 || col_text(r, 0, 0, rop, sizeof(rop)) ||
          col_text(r, 0, 1, actor, sizeof(actor)) || strcmp(actor, "uid:0") ||
          col_text(r, 0, 2, rreq, sizeof(rreq)) ||
          kb_store_vault_reseal_operation_id_from_hex(rop, b->operation_id) ||
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
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      else
         b->state = KB_STORE_VAULT_REWRAP_COMPLETED;
   }
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int kb_store_vault_operator_current_check_page(const kb_store_vault_rewrap_cursor_t *a, int limit,
                                               kb_store_vault_rewrap_check_t *rows, size_t cap,
                                               size_t *count, kb_store_vault_rewrap_cursor_t *next,
                                               int64_t *total)
{
   static const uint8_t empty = 0;
   if (rows && cap <= 128)
      kb_store_vault_rewrap_check_clear(rows, cap);
   if (count)
      *count = 0;
   if (next)
      kb_store_vault_rewrap_cursor_clear(next);
   if (total)
      *total = 0;
   if (!a || a->len > 640 || !rows || !count || !next || !total || limit < 1 || limit > 128 ||
       cap < (size_t)limit || cap > 128)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char lb[16];
   snprintf(lb, sizeof(lb), "%d", limit);
   uint32_t ty[] = {17, 23};
   const char *v[] = {(char *)(a->len ? a->bytes : &empty), lb};
   int lens[] = {(int)a->len, 0}, fmts[] = {1, 0};
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_current_check_page($1,$2)", 2, ty, v, lens,
                         fmts, &r);
   *next = *a;
   if (!rc)
   {
      int n = result_rows(r);
      if (result_columns(r) != 4 || n > limit)
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      else if (n == 1 && result_null(r, 0, 0))
      {
         int64_t tc;
         if (!result_null(r, 0, 1) || result_null(r, 0, 2) ||
             result_length(r, 0, 2) != (int)a->len ||
             CRYPTO_memcmp(result_value(r, 0, 2), a->bytes, a->len) || col_i64(r, 0, 3, &tc) ||
             tc < 0)
            rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
         else
            *total = tc;
      }
      else
         for (int i = 0; !rc && i < n; i++)
         {
            kb_store_vault_rewrap_check_t *x = &rows[i];
            int kn = result_length(r, i, 1), cn = result_length(r, i, 2);
            uint8_t *cur = (uint8_t *)result_value(r, i, 2);
            int64_t tc;
            if (result_null(r, i, 0) || col_text(r, i, 0, x->principal, sizeof(x->principal)) ||
                result_null(r, i, 1) || result_null(r, i, 2) || (kn != 0 && kn != 40) || cn < 1 ||
                cn > 640 || strlen(x->principal) != (size_t)cn || memcmp(x->principal, cur, cn) ||
                cursor_cmp(cur, cn, next->bytes, next->len) <= 0 || col_i64(r, i, 3, &tc) ||
                tc < 0 || (i && tc != *total))
               rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
            else
            {
               if (kn)
                  memcpy(x->kek_check, result_value(r, i, 1), kn);
               x->kek_check_len = kn;
               memcpy(next->bytes, cur, cn);
               next->len = cn;
               *total = tc;
               (*count)++;
            }
         }
   }
   aimee_postgres_result_free(r);
   if (rc)
   {
      kb_store_vault_rewrap_check_clear(rows, cap);
      *count = 0;
      kb_store_vault_rewrap_cursor_clear(next);
      *total = 0;
   }
   return rc;
}
static int open_decode(aimee_postgres_result_t *r, kb_store_vault_operator_open_result_t *out)
{
   char eid[65];
   return result_rows(r) != 1 || result_columns(r) != 4 || col_i64(r, 0, 0, &out->opened_epoch) ||
                  col_i64(r, 0, 1, &out->opened_fence) || col_text(r, 0, 2, eid, sizeof(eid)) ||
                  hex_bytes(eid, 32, out->event_id) || col_blob(r, 0, 3, out->row_hash, 32) ||
                  out->opened_epoch < 1 || out->opened_fence < 1
              ? -1
              : 0;
}
int kb_store_vault_operator_open_completed(const kb_store_vault_operator_completed_t *c,
                                           kb_store_vault_operator_open_result_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!c || !out || c->binding.state != KB_STORE_VAULT_REWRAP_COMPLETED)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char rh[33], oh[33], eb[32], fb[32];
   bytes_hex(c->binding.request_id, 16, rh);
   op_hex(c->binding.operation_id, oh);
   snprintf(eb, sizeof(eb), "%lld", (long long)c->binding.seal_epoch);
   snprintf(fb, sizeof(fb), "%lld", (long long)c->binding.fencing_token);
   uint32_t ty[] = {25, 25, 25, 20, 20, 17, 17, 17};
   const char *v[] = {"uid:0",
                      rh,
                      oh,
                      eb,
                      fb,
                      (char *)c->receipt_digest,
                      (char *)c->inventory_digest,
                      (char *)c->stage_digest};
   int l[] = {0, 0, 0, 0, 0, 32, 32, 32}, f[] = {0, 0, 0, 0, 0, 1, 1, 1};
   aimee_postgres_result_t *r = NULL;
   int rc =
       direct_query("SELECT * FROM " API "org_vault_rewrap_open_completed($1,$2,$3,$4,$5,$6,$7,$8)",
                    8, ty, v, l, f, &r);
   if (!rc && open_decode(r, out))
      rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int kb_store_vault_operator_open_idle(const uint8_t req[16], int64_t epoch, int64_t fence,
                                      int64_t marker, kb_store_vault_operator_open_result_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!req || !out || epoch < 1 || fence < 1 || marker < 0 || marker > fence)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char rh[33], eb[32], fb[32], mb[32];
   bytes_hex(req, 16, rh);
   snprintf(eb, sizeof(eb), "%lld", (long long)epoch);
   snprintf(fb, sizeof(fb), "%lld", (long long)fence);
   snprintf(mb, sizeof(mb), "%lld", (long long)marker);
   uint32_t ty[] = {25, 25, 20, 20, 20};
   const char *v[] = {"uid:0", rh, eb, fb, mb};
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_open_idle($1,$2,$3,$4,$5)", 5, ty, v, NULL,
                         NULL, &r);
   if (!rc && open_decode(r, out))
      rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}
int kb_store_vault_operator_open_event(const uint8_t id[32],
                                       kb_store_vault_operator_open_event_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!id || !out)
      return KB_STORE_VAULT_REWRAP_INVALID;
   char eh[65];
   bytes_hex(id, 32, eh);
   uint32_t ty[] = {25};
   const char *v[] = {eh};
   aimee_postgres_result_t *r = NULL;
   int rc = direct_query("SELECT * FROM " API "org_vault_open_event($1)", 1, ty, v, NULL, NULL, &r);
   if (!rc)
   {
      char re[65], kind[32], op[33], req[33], actor[16];
      if (result_rows(r) != 1 || result_columns(r) != 9 || col_text(r, 0, 0, re, sizeof(re)) ||
          hex_bytes(re, 32, out->opened.event_id) || CRYPTO_memcmp(id, out->opened.event_id, 32) ||
          col_text(r, 0, 1, kind, sizeof(kind)) || col_text(r, 0, 3, req, sizeof(req)) ||
          hex_bytes(req, 16, out->request_id) || col_text(r, 0, 4, actor, sizeof(actor)) ||
          strcmp(actor, "uid:0") || col_i64(r, 0, 6, &out->opened.opened_epoch) ||
          col_i64(r, 0, 7, &out->opened.opened_fence) ||
          col_blob(r, 0, 8, out->opened.row_hash, 32))
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      else if (!strcmp(kind, "completed_opened"))
      {
         out->completed_open = 1;
         out->operation_present = 1;
         if (result_null(r, 0, 2) || col_text(r, 0, 2, op, sizeof(op)) ||
             kb_store_vault_reseal_operation_id_from_hex(op, out->operation_id) ||
             col_i64(r, 0, 5, &out->operation_fence))
            rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
      }
      else if (strcmp(kind, "idle_opened") || !result_null(r, 0, 2) || !result_null(r, 0, 5))
         rc = KB_STORE_VAULT_REWRAP_INTEGRITY;
   }
   aimee_postgres_result_free(r);
   if (rc)
      OPENSSL_cleanse(out, sizeof(*out));
   return rc;
}

const kb_store_vault_rewrap_ops_t kb_store_vault_operator_rewrap_ops = {
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
