#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vault_operator_status_runtime.h"

#include <libpq-fe.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(pthread_mutex_t) <=
                   sizeof(((db2_vault_operator_runtime_t *)0)->mutex_storage),
               "vault operator mutex storage too small");

static pthread_mutex_t *runtime_mutex(db2_vault_operator_runtime_t *r)
{
   return (pthread_mutex_t *)(void *)r->mutex_storage;
}

static void set_error(char *out, size_t cap, const char *message)
{
   if (out && cap)
      snprintf(out, cap, "%s", message);
}

static int64_t now_ms(void)
{
   struct timespec t;
   return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ? (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000
                                                  : -1;
}

static int deadline_new(int64_t *out)
{
   int64_t now = now_ms();
   if (now < 0 || now > INT64_MAX - 2000)
      return -1;
   *out = now + 2000;
   return 0;
}

static int wait_fd(int fd, short events, int64_t deadline)
{
   for (;;)
   {
      int64_t now = now_ms();
      if (now < 0 || now >= deadline)
         return -1;
      int64_t left = deadline - now;
      struct pollfd p = {.fd = fd, .events = events};
      int rc = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
      if (rc < 0 && errno == EINTR)
         continue;
      return rc == 1 && !(p.revents & (POLLERR | POLLNVAL)) && (p.revents & (events | POLLHUP))
                 ? 0
                 : -1;
   }
}

static int mutex_until(pthread_mutex_t *mutex, int64_t deadline)
{
   for (;;)
   {
      int rc = pthread_mutex_trylock(mutex);
      if (rc == 0)
         return 0;
      if (rc != EBUSY)
         return -1;
      int64_t now = now_ms();
      if (now < 0 || now >= deadline)
         return -1;
      struct timespec pause = {.tv_nsec = 1000000};
      (void)nanosleep(&pause, NULL);
   }
}

static const char *conninfo_value(PQconninfoOption *options, const char *keyword)
{
   for (size_t i = 0; options && options[i].keyword; ++i)
      if (!strcmp(options[i].keyword, keyword))
         return options[i].val && options[i].val[0] ? options[i].val : NULL;
   return NULL;
}

static int numeric_host(const char *host)
{
   unsigned char address[sizeof(struct in6_addr)];
   return host && !strchr(host, ',') &&
          (inet_pton(AF_INET, host, address) == 1 || inet_pton(AF_INET6, host, address) == 1);
}

static int dns_identity(const char *host)
{
   size_t length = host ? strlen(host) : 0;
   if (!length || length > 253 || host[0] == '.' || host[length - 1] == '.' || host[0] == '-' ||
       host[length - 1] == '-')
      return 0;
   for (size_t i = 0; i < length; ++i)
   {
      unsigned char c = (unsigned char)host[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.'))
         return 0;
      if ((c == '.' && ((i > 0 && host[i - 1] == '-') || (i + 1 < length && host[i + 1] == '-'))) ||
          (c == '.' && i + 1 < length && host[i + 1] == '.'))
         return 0;
   }
   return 1;
}

#if defined(AIMEE_P7_D3_INTEGRATION_TEST_OVERRIDE)
static int loopback_host(const char *host)
{
   return host && (!strcmp(host, "127.0.0.1") || !strcmp(host, "::1"));
}
#endif

static int conninfo_policy(PQconninfoOption *options, int *tcp_out)
{
   const char *host = conninfo_value(options, "host");
   const char *hostaddr = conninfo_value(options, "hostaddr");
   const char *sslmode = conninfo_value(options, "sslmode");
   int tcp = host && host[0] != '/';
   if (tcp_out)
      *tcp_out = 0;
   /* A DNS certificate identity is permitted only with one separately pinned
    * numeric address.  Thus libpq never performs unbounded name resolution. */
   if ((host && strchr(host, ',')) || (hostaddr && !numeric_host(hostaddr)) ||
       (hostaddr && (!host || host[0] == '/')) ||
       (tcp && !numeric_host(host) && !(dns_identity(host) && hostaddr)))
      return -1;
   if (tcp)
   {
#if defined(AIMEE_P7_D3_INTEGRATION_TEST_OVERRIDE)
      if (!loopback_host(host) && (!sslmode || strcmp(sslmode, "verify-full")))
         return -1;
#else
      if (!sslmode || strcmp(sslmode, "verify-full"))
         return -1;
#endif
   }
   if (tcp_out)
      *tcp_out = tcp;
   return 0;
}

int db2_vault_operator_conninfo_allowed_for_test(const char *conninfo, int *tcp_out)
{
   char *parse_error = NULL;
   PQconninfoOption *options = PQconninfoParse(conninfo, &parse_error);
   PQfreemem(parse_error);
   if (!options)
      return -1;
   int rc = conninfo_policy(options, tcp_out);
   PQconninfoFree(options);
   return rc;
}

static void pq_cancel_async(PGconn *connection, int64_t deadline)
{
   PGcancelConn *cancel = PQcancelCreate(connection);
   if (!cancel)
      return;
   if (PQcancelStart(cancel) == 1)
      for (;;)
      {
         PostgresPollingStatusType state = PQcancelPoll(cancel);
         if (state == PGRES_POLLING_OK || state == PGRES_POLLING_FAILED)
            break;
         short events = state == PGRES_POLLING_READING ? POLLIN : POLLOUT;
         if (wait_fd(PQcancelSocket(cancel), events, deadline) != 0)
            break;
      }
   PQcancelFinish(cancel);
}

static void *prod_open(void *context, const char *conninfo, int64_t deadline, char *err,
                       size_t errlen)
{
   (void)context;
   char *parse_error = NULL;
   PQconninfoOption *options = PQconninfoParse(conninfo, &parse_error);
   if (!options)
   {
      set_error(err, errlen, parse_error ? parse_error : "invalid PostgreSQL URL");
      PQfreemem(parse_error);
      return NULL;
   }
   int tcp = 0;
   if (conninfo_policy(options, &tcp) != 0)
   {
      PQconninfoFree(options);
      set_error(err, errlen, "vault orchestrator database transport policy refused");
      return NULL;
   }
   int require_ssl = tcp;
#if defined(AIMEE_P7_D3_INTEGRATION_TEST_OVERRIDE)
   if (loopback_host(conninfo_value(options, "host")))
      require_ssl = 0;
#endif
   size_t count = 0;
   while (options[count].keyword)
      ++count;
   const char **keywords = calloc(count + 3, sizeof(*keywords));
   const char **values = calloc(count + 3, sizeof(*values));
   if (!keywords || !values)
   {
      free(keywords);
      free(values);
      PQconninfoFree(options);
      return NULL;
   }
   size_t used = 0;
   for (size_t i = 0; i < count; ++i)
      if (options[i].val && options[i].val[0] && strcmp(options[i].keyword, "connect_timeout"))
      {
         keywords[used] = options[i].keyword;
         values[used++] = options[i].val;
      }
   keywords[used] = "connect_timeout";
   values[used++] = "2";
   if (!conninfo_value(options, "host"))
   {
      keywords[used] = "host";
      values[used++] = "/var/run/postgresql";
   }
   PGconn *connection = PQconnectStartParams(keywords, values, 0);
   free(keywords);
   free(values);
   PQconninfoFree(options);
   if (!connection)
      return NULL;
   if (PQsetnonblocking(connection, 1) != 0)
      goto fail;
   for (;;)
   {
      PostgresPollingStatusType state = PQconnectPoll(connection);
      if (state == PGRES_POLLING_OK)
      {
         if (require_ssl && !PQsslInUse(connection))
            goto fail;
         return connection;
      }
      if (state == PGRES_POLLING_FAILED)
         goto fail;
      short events = state == PGRES_POLLING_READING ? POLLIN : POLLOUT;
      if (wait_fd(PQsocket(connection), events, deadline))
         goto fail;
   }
fail:
   set_error(err, errlen, "vault orchestrator database connection unavailable");
   PQfinish(connection);
   return NULL;
}

static void prod_close(void *context, void *connection)
{
   (void)context;
   PQfinish((PGconn *)connection);
}

static int prod_query(void *context, void *opaque, const char *sql, int64_t deadline,
                      db2_vault_operator_db_result_t *out, char *err, size_t errlen)
{
   (void)context;
   PGconn *connection = opaque;
   memset(out, 0, sizeof(*out));
   /* Reserve the final 100ms of the absolute operation deadline for PG17's
    * asynchronous cancel handshake.  Cancellation remains best effort. */
   int64_t query_deadline = deadline >= 100 ? deadline - 100 : deadline;
   int64_t now = now_ms();
   if (now < 0 || now >= query_deadline)
      goto timeout;
   if (!PQsendQuery(connection, sql))
      goto fail;
   int flush_result;
   while ((flush_result = PQflush(connection)) == 1)
      if (wait_fd(PQsocket(connection), POLLOUT, query_deadline))
         goto timeout;
   if (flush_result < 0)
      goto fail;
   for (;;)
   {
      if (PQconsumeInput(connection) == 0)
         goto fail;
      if (!PQisBusy(connection))
         break;
      if (wait_fd(PQsocket(connection), POLLIN, query_deadline))
         goto timeout;
   }
   PGresult *result = PQgetResult(connection);
   if (!result)
      goto fail;
   ExecStatusType status = PQresultStatus(result);
   if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
   {
      const char *sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
      int integrity =
          sqlstate && !strcmp(sqlstate, "55000") &&
          strstr(sql, "aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()") != NULL;
      PQclear(result);
      PGresult *remaining;
      while ((remaining = PQgetResult(connection)) != NULL)
         PQclear(remaining);
      if (integrity)
         return DB2_VAULT_OPERATOR_INTEGRITY;
      goto fail;
   }
   int rows = PQntuples(result), cols = PQnfields(result);
   if (rows < 0 || rows > 2 || cols < 0 || cols > 11)
   {
      PQclear(result);
      goto fail;
   }
   out->rows = (unsigned)rows;
   out->columns = (unsigned)cols;
   for (int row = 0; row < rows; ++row)
      for (int col = 0; col < cols; ++col)
      {
         if (PQgetisnull(result, row, col))
            out->is_null[row][col] = 1;
         else
         {
            int len = PQgetlength(result, row, col);
            if (len < 0 || len > 128)
            {
               PQclear(result);
               goto fail;
            }
            memcpy(out->value[row][col], PQgetvalue(result, row, col), (size_t)len);
            out->value[row][col][len] = '\0';
         }
      }
   PQclear(result);
   PGresult *surplus = PQgetResult(connection);
   if (surplus)
   {
      PQclear(surplus);
      goto fail;
   }
   return 0;
timeout:
   pq_cancel_async(connection, deadline);
fail:
   set_error(err, errlen, "vault orchestrator database query unavailable");
   return -1;
}

static int prod_idle(void *context, void *connection)
{
   (void)context;
   return PQtransactionStatus((PGconn *)connection) == PQTRANS_IDLE;
}

static const db2_vault_operator_db_vtable_t production_database = {prod_open, prod_close,
                                                                   prod_query, prod_idle};

static int bool_text(const char *s, int *out)
{
   if (!strcmp(s, "t") || !strcmp(s, "1"))
      *out = 1;
   else if (!strcmp(s, "f") || !strcmp(s, "0"))
      *out = 0;
   else
      return -1;
   return 0;
}

static void discard(db2_vault_operator_runtime_t *r)
{
   if (r->connection)
      r->database->close(r->database_context, r->connection);
   r->connection = NULL;
   r->transaction_active = 0;
}

static int query(db2_vault_operator_runtime_t *r, const char *sql, int64_t deadline,
                 db2_vault_operator_db_result_t *result)
{
   char error[256] = "";
   int64_t now = now_ms();
   if (now < 0 || now >= deadline)
      return -1;
   return r->database->query(r->database_context, r->connection, sql, deadline, result, error,
                             sizeof(error));
}

static int command(db2_vault_operator_runtime_t *r, const char *sql, int64_t deadline)
{
   db2_vault_operator_db_result_t result;
   return query(r, sql, deadline, &result) || result.rows != 0 || result.columns != 0;
}

static int session_assert(db2_vault_operator_runtime_t *r, int after_role, int64_t deadline)
{
   static const char before[] =
       "SELECT (session_user='aimee_kb_vault_orchestrator_login' AND current_user=session_user AND "
       "l.rolcanlogin AND NOT l.rolinherit AND NOT l.rolsuper AND NOT l.rolbypassrls AND NOT "
       "l.rolcreatedb AND NOT l.rolcreaterole AND NOT l.rolreplication AND "
       "pg_catalog.current_setting('search_path')='pg_catalog, pg_temp' AND "
       "pg_catalog.current_setting('row_security')='on' AND "
       "pg_catalog.current_setting('statement_timeout')='1900ms' AND "
       "pg_catalog.current_setting('lock_timeout')='1900ms' AND "
       "pg_catalog.has_database_privilege(session_user,pg_catalog.current_database(),'CONNECT') "
       "AND NOT pg_catalog.has_database_privilege(session_user,pg_catalog.current_database(),"
       "'CREATE') AND NOT pg_catalog.has_database_privilege(session_user,"
       "pg_catalog.current_database(),'TEMPORARY') AND "
       "pg_catalog.pg_has_role(session_user,'aimee_kb_vault_orchestrator','MEMBER') AND (SELECT "
       "count(*) FROM pg_catalog.pg_auth_members m WHERE m.member=l.oid)=1 AND NOT EXISTS (SELECT "
       "1 FROM pg_catalog.pg_auth_members m JOIN pg_catalog.pg_roles granted ON "
       "granted.oid=m.roleid WHERE m.member=l.oid AND "
       "granted.rolname<>'aimee_kb_vault_orchestrator') AND NOT EXISTS (SELECT 1 FROM "
       "pg_catalog.pg_auth_members m WHERE m.roleid=(SELECT oid FROM pg_catalog.pg_roles WHERE "
       "rolname='aimee_kb_vault_orchestrator') AND m.member<>l.oid) AND NOT EXISTS (SELECT 1 FROM "
       "pg_catalog.pg_namespace n WHERE pg_catalog.left(n.nspname,3)<>'pg_' AND "
       "n.nspname<>'information_schema' AND (pg_catalog.has_schema_privilege(session_user,n.oid,"
       "'USAGE') OR pg_catalog.has_schema_privilege(session_user,n.oid,'CREATE'))) AND NOT EXISTS "
       "(SELECT 1 FROM pg_catalog.pg_namespace n CROSS JOIN LATERAL "
       "pg_catalog.aclexplode(n.nspacl) acl WHERE acl.grantee=l.oid) AND NOT EXISTS (SELECT 1 FROM "
       "pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace CROSS JOIN "
       "LATERAL pg_catalog.aclexplode(c.relacl) acl WHERE acl.grantee=l.oid) AND NOT EXISTS "
       "(SELECT 1 FROM "
       "pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace CROSS JOIN "
       "LATERAL pg_catalog.aclexplode(p.proacl) acl WHERE acl.grantee=l.oid) AND "
       "NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON "
       "n.oid=c.relnamespace WHERE "
       "n.nspname='public' AND c.relowner=l.oid) AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_proc "
       "p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname='public' AND "
       "p.proowner=l.oid) AND l.oid<>(SELECT d.datdba FROM pg_catalog.pg_database d WHERE "
       "d.datname=pg_catalog.current_database())) FROM pg_catalog.pg_roles l WHERE "
       "l.rolname=session_user";
   static const char after[] =
       "SELECT (session_user='aimee_kb_vault_orchestrator_login' AND "
       "current_user='aimee_kb_vault_orchestrator' AND NOT o.rolcanlogin AND NOT o.rolinherit AND "
       "NOT o.rolsuper AND NOT o.rolbypassrls AND NOT o.rolcreatedb AND NOT o.rolcreaterole AND "
       "NOT o.rolreplication AND pg_catalog.current_setting('search_path')='pg_catalog, pg_temp' "
       "AND pg_catalog.current_setting('row_security')='on' AND "
       "pg_catalog.current_setting('statement_timeout')='1900ms' AND "
       "pg_catalog.current_setting('lock_timeout')='1900ms' AND "
       "pg_catalog.has_database_privilege(current_user,pg_catalog.current_database(),'CONNECT') "
       "AND NOT pg_catalog.has_database_privilege(current_user,pg_catalog.current_database(),"
       "'CREATE') AND NOT pg_catalog.has_database_privilege(current_user,"
       "pg_catalog.current_database(),'TEMPORARY') AND o.oid<>(SELECT d.datdba FROM "
       "pg_catalog.pg_database d WHERE d.datname=pg_catalog.current_database()) AND "
       "pg_catalog.pg_has_role(session_user,current_user,'MEMBER') AND "
       "NOT pg_catalog.has_schema_privilege(current_user,'public','USAGE') AND "
       "pg_catalog.has_schema_privilege(current_user,"
       "'aimee_kb_vault_orchestrator_api','USAGE') AND "
       "(SELECT count(*) FROM pg_catalog.pg_namespace n WHERE "
       "pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema' AND "
       "pg_catalog.has_schema_privilege(current_user,n.oid,'USAGE'))=1 AND "
       "NOT pg_catalog.has_schema_privilege(current_user,"
       "'aimee_kb_vault_orchestrator_api','CREATE') AND "
       "pg_catalog.has_function_privilege(current_user,"
       "'aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()',"
       "'EXECUTE') AND (SELECT count(*) FROM pg_catalog.pg_proc p JOIN "
       "pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE "
       "pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema' AND "
       "pg_catalog.has_schema_privilege(current_user,n.oid,'USAGE') AND "
       "pg_catalog.has_function_privilege(current_user,p.oid,'EXECUTE'))=1 AND "
       "NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON "
       "n.oid=c.relnamespace WHERE pg_catalog.left(n.nspname,3)<>'pg_' AND "
       "n.nspname<>'information_schema' AND CASE WHEN c.relkind IN ('r','p','v','m','f') THEN "
       "pg_catalog.has_table_privilege(current_user,c.oid,"
       "'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') ELSE false END) AND NOT EXISTS "
       "(SELECT 1 FROM "
       "pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE "
       "pg_catalog.left(n.nspname,3)<>'pg_' AND n.nspname<>'information_schema' AND "
       "CASE WHEN c.relkind='S' THEN pg_catalog.has_sequence_privilege(current_user,c.oid,"
       "'USAGE,SELECT,UPDATE') ELSE false END) AND "
       "NOT EXISTS (SELECT 1 FROM pg_catalog.pg_namespace n CROSS JOIN LATERAL "
       "pg_catalog.aclexplode(n.nspacl) acl WHERE acl.grantee=o.oid AND "
       "n.nspname<>'aimee_kb_vault_orchestrator_api') AND NOT EXISTS (SELECT 1 FROM "
       "pg_catalog.pg_class c CROSS JOIN LATERAL pg_catalog.aclexplode(c.relacl) acl WHERE "
       "acl.grantee=o.oid) AND (SELECT count(*) FROM pg_catalog.pg_proc p CROSS JOIN LATERAL "
       "pg_catalog.aclexplode(p.proacl) acl WHERE acl.grantee=o.oid)=1 AND "
       "(SELECT count(*) FROM pg_catalog.pg_auth_members membership WHERE "
       "membership.roleid=o.oid AND membership.member=(SELECT oid FROM pg_catalog.pg_roles WHERE "
       "rolname=session_user))=1 AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_auth_members "
       "membership WHERE membership.roleid=o.oid AND membership.member<>(SELECT oid FROM "
       "pg_catalog.pg_roles WHERE rolname=session_user)) AND "
       "NOT EXISTS (SELECT 1 FROM pg_catalog.pg_namespace n WHERE n.nspowner=o.oid) AND NOT EXISTS "
       "(SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON "
       "n.oid=c.relnamespace WHERE c.relowner=o.oid) AND NOT EXISTS (SELECT 1 FROM "
       "pg_catalog.pg_proc p JOIN "
       "pg_catalog.pg_namespace n ON n.oid=p.pronamespace WHERE p.proowner=o.oid) AND NOT EXISTS "
       "(SELECT 1 FROM "
       "pg_catalog.pg_auth_members membership WHERE membership.member=o.oid)) FROM "
       "pg_catalog.pg_roles o "
       "WHERE o.rolname=current_user";
   db2_vault_operator_db_result_t result;
   int value = 0;
   return query(r, after_role ? after : before, deadline, &result) || result.rows != 1 ||
          result.columns != 1 || result.is_null[0][0] || bool_text(result.value[0][0], &value) ||
          !value;
}

int db2_vault_operator_runtime_open_with_vtable(db2_vault_operator_runtime_t *r,
                                                const char *conninfo,
                                                const db2_vault_operator_db_vtable_t *db,
                                                void *db_context, char *err, size_t errlen)
{
   if (!r || !conninfo || !*conninfo || !db || !db->open || !db->close || !db->query ||
       !db->transaction_idle)
      return DB2_VAULT_OPERATOR_UNAVAILABLE;
   memset(r, 0, sizeof(*r));
   r->database = db;
   r->database_context = db_context;
   if (pthread_mutex_init(runtime_mutex(r), NULL))
      return DB2_VAULT_OPERATOR_UNAVAILABLE;
   r->mutex_initialized = 1;
   int64_t deadline;
   if (deadline_new(&deadline))
      goto fail;
   r->connection = db->open(db_context, conninfo, deadline, err, errlen);
   if (!r->connection || command(r, "SET search_path = pg_catalog, pg_temp", deadline) ||
       command(r, "SET row_security = on", deadline) ||
       command(r, "SET statement_timeout = '1900ms'", deadline) ||
       command(r, "SET lock_timeout = '1900ms'", deadline) || session_assert(r, 0, deadline) ||
       command(r, "SET ROLE aimee_kb_vault_orchestrator", deadline) ||
       session_assert(r, 1, deadline))
      goto fail;
   return DB2_VAULT_OPERATOR_OK;
fail:
   discard(r);
   pthread_mutex_destroy(runtime_mutex(r));
   r->mutex_initialized = 0;
   set_error(err, errlen, "vault orchestrator database authority assertion failed");
   return DB2_VAULT_OPERATOR_UNAVAILABLE;
}

int db2_vault_operator_runtime_open(db2_vault_operator_runtime_t *r, const char *conninfo,
                                    char *err, size_t errlen)
{
   return db2_vault_operator_runtime_open_with_vtable(r, conninfo, &production_database, NULL, err,
                                                      errlen);
}

void db2_vault_operator_runtime_close(db2_vault_operator_runtime_t *r)
{
   if (!r)
      return;
   if (r->mutex_initialized)
   {
      int64_t deadline;
      if (!deadline_new(&deadline) && !mutex_until(runtime_mutex(r), deadline))
      {
         discard(r);
         pthread_mutex_unlock(runtime_mutex(r));
      }
      else
         return;
      pthread_mutex_destroy(runtime_mutex(r));
   }
   memset(r, 0, sizeof(*r));
}

static int64_t parse_i64(const char *s, int64_t *out)
{
   if (!s || !*s)
      return -1;
   errno = 0;
   char *end = NULL;
   long long value = strtoll(s, &end, 10);
   if (errno || !end || *end)
      return -1;
   *out = (int64_t)value;
   return 0;
}
static int lower_token(const char *s)
{
   size_t n = s ? strlen(s) : 65;
   if (n > 64)
      return 0;
   if (!n)
      return 1;
   if (s[0] < 'a' || s[0] > 'z')
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
         return 0;
   return 1;
}
static int nibble(unsigned char c)
{
   return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
}
static int decode_id(const char *s, unsigned char out[16])
{
   if (!s || strlen(s) != 32)
      return -1;
   for (size_t i = 0; i < 16; ++i)
   {
      int a = nibble(s[i * 2]), b = nibble(s[i * 2 + 1]);
      if (a < 0 || b < 0)
         return -1;
      out[i] = (unsigned char)(a * 16 + b);
   }
   return 0;
}
static db2_vault_operation_state_t decode_state(const char *s)
{
   static const char *names[] = {
       "",         "preparing", "custody_prepared", "wraps_staged", "reseal_committing",
       "resealed", "promoted",  "completed",        "aborted",      "recovery_required"};
   for (int i = 1; i <= 9; ++i)
      if (!strcmp(s, names[i]))
         return (db2_vault_operation_state_t)i;
   return 0;
}
static int decode(const db2_vault_operator_db_result_t *r, db2_vault_operator_snapshot_t *out)
{
   if (r->rows != 1 || r->columns != 11)
      return -1;
   db2_vault_operator_snapshot_t v;
   memset(&v, 0, sizeof(v));
   for (int i = 0; i < 4; ++i)
      if (r->is_null[0][i])
         return -1;
   if (parse_i64(r->value[0][0], &v.seal_epoch) || bool_text(r->value[0][1], &v.sealed) ||
       parse_i64(r->value[0][2], &v.control_fence) ||
       parse_i64(r->value[0][3], &v.last_opened_fence) || v.seal_epoch < 1 || v.control_fence < 1 ||
       v.last_opened_fence < 0 || v.last_opened_fence > v.control_fence)
      return -1;
   int absent = r->is_null[0][4];
   for (int i = 4; i < 11; ++i)
      if (r->is_null[0][i] != absent)
         return -1;
   if (!absent)
   {
      v.operation_state = decode_state(r->value[0][5]);
      if (!v.operation_state || decode_id(r->value[0][4], v.operation_id) ||
          parse_i64(r->value[0][6], &v.operation_seal_epoch) ||
          parse_i64(r->value[0][7], &v.operation_fence) ||
          parse_i64(r->value[0][8], &v.old_generation) ||
          parse_i64(r->value[0][9], &v.new_generation) || v.operation_seal_epoch < 1 ||
          v.operation_fence < 1 || v.old_generation < 0 || v.old_generation == INT64_MAX ||
          v.new_generation != v.old_generation + 1 || !lower_token(r->value[0][10]) ||
          ((v.operation_state == DB2_VAULT_OPERATION_ABORTED ||
            v.operation_state == DB2_VAULT_OPERATION_RECOVERY_REQUIRED) !=
           (r->value[0][10][0] != 0)))
         return -1;
      size_t failure_len = strlen(r->value[0][10]);
      memcpy(v.failure_class, r->value[0][10], failure_len + 1);
      v.operation_present = 1;
   }
   *out = v;
   return 0;
}

static int snapshot_locked(db2_vault_operator_runtime_t *r, int64_t deadline,
                           db2_vault_operator_snapshot_t *out)
{
   if (!r->connection || r->transaction_active ||
       !r->database->transaction_idle(r->database_context, r->connection))
      goto unavailable;
   if (session_assert(r, 1, deadline) || command(r, "BEGIN", deadline))
      goto unavailable;
   r->transaction_active = 1;
   db2_vault_operator_db_result_t result;
   int q = query(r,
                 "SELECT "
                 "seal_epoch,sealed,control_fence,last_opened_fence,operation_id,operation_state,"
                 "operation_seal_epoch,operation_fence,old_generation,new_generation,failure_class "
                 "FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()",
                 deadline, &result);
   int rc =
       q == DB2_VAULT_OPERATOR_INTEGRITY
           ? DB2_VAULT_OPERATOR_INTEGRITY
           : (q ? DB2_VAULT_OPERATOR_UNAVAILABLE
                : (decode(&result, out) ? DB2_VAULT_OPERATOR_INTEGRITY : DB2_VAULT_OPERATOR_OK));
   if (command(r, rc == DB2_VAULT_OPERATOR_OK ? "COMMIT" : "ROLLBACK", deadline) &&
       rc != DB2_VAULT_OPERATOR_INTEGRITY)
      rc = DB2_VAULT_OPERATOR_UNAVAILABLE;
   r->transaction_active = 0;
   if (rc != DB2_VAULT_OPERATOR_OK)
   {
      discard(r);
      return rc;
   }
   return rc;
unavailable:
   discard(r);
   return DB2_VAULT_OPERATOR_UNAVAILABLE;
}

int db2_vault_operator_runtime_snapshot(db2_vault_operator_runtime_t *r,
                                        db2_vault_operator_snapshot_t *out)
{
   int64_t deadline;
   if (!r || !out || !r->mutex_initialized || deadline_new(&deadline) ||
       mutex_until(runtime_mutex(r), deadline))
      return DB2_VAULT_OPERATOR_UNAVAILABLE;
   memset(out, 0, sizeof(*out));
   int rc = snapshot_locked(r, deadline, out);
   pthread_mutex_unlock(runtime_mutex(r));
   return rc;
}
int db2_vault_operator_snapshot_equal(const db2_vault_operator_snapshot_t *a,
                                      const db2_vault_operator_snapshot_t *b)
{
   return a && b && !memcmp(a, b, sizeof(*a));
}

static int classify(const db2_vault_operator_snapshot_t *s, db2_vault_provider_status_t p,
                    db2_vault_operator_status_t *o)
{
   if (p == DB2_VAULT_PROVIDER_MALFORMED)
      goto bad;
   if (s->operation_present)
   {
      if (s->operation_state == DB2_VAULT_OPERATION_RECOVERY_REQUIRED)
      {
         o->state = DB2_VAULT_STATE_RECOVERY_REQUIRED;
         o->remediation = p == DB2_VAULT_PROVIDER_UNAVAILABLE ? DB2_VAULT_REMEDIATION_BACKEND
                                                              : DB2_VAULT_REMEDIATION_RECOVER;
         return 0;
      }
      if (s->operation_state == DB2_VAULT_OPERATION_COMPLETED &&
          p == DB2_VAULT_PROVIDER_AVAILABLE_SEALED)
      {
         o->state = DB2_VAULT_STATE_COMPLETED_SEALED;
         o->remediation = DB2_VAULT_REMEDIATION_FINALIZE;
         return 0;
      }
      if (s->operation_state >= DB2_VAULT_OPERATION_PREPARING &&
          s->operation_state <= DB2_VAULT_OPERATION_PROMOTED)
      {
         o->state = DB2_VAULT_STATE_RESUME_REQUIRED;
         o->remediation = p == DB2_VAULT_PROVIDER_UNAVAILABLE ? DB2_VAULT_REMEDIATION_BACKEND
                                                              : DB2_VAULT_REMEDIATION_RESUME;
         return 0;
      }
      goto bad;
   }
   if (s->sealed && p == DB2_VAULT_PROVIDER_AVAILABLE_SEALED)
   {
      o->state = DB2_VAULT_STATE_SEALED_IDLE;
      o->remediation = DB2_VAULT_REMEDIATION_UNSEAL;
      return 0;
   }
   if (s->sealed && p == DB2_VAULT_PROVIDER_UNAVAILABLE)
   {
      o->state = DB2_VAULT_STATE_BACKEND_UNAVAILABLE;
      o->remediation = DB2_VAULT_REMEDIATION_BACKEND;
      return 0;
   }
   if (!s->sealed && p == DB2_VAULT_PROVIDER_AVAILABLE_UNSEALED)
   {
      o->state = DB2_VAULT_STATE_OPERATIONAL;
      o->remediation = DB2_VAULT_REMEDIATION_NONE;
      return 0;
   }
   if (!s->sealed && p == DB2_VAULT_PROVIDER_AVAILABLE_SEALED)
   {
      o->state = DB2_VAULT_STATE_LOCAL_UNSEAL_REQUIRED;
      o->remediation = DB2_VAULT_REMEDIATION_UNSEAL;
      return 0;
   }
   if (!s->sealed && p == DB2_VAULT_PROVIDER_UNAVAILABLE)
   {
      o->state = DB2_VAULT_STATE_BACKEND_UNAVAILABLE;
      o->remediation = DB2_VAULT_REMEDIATION_BACKEND;
      return 0;
   }
bad:
   o->state = DB2_VAULT_STATE_INTEGRITY_FAILURE;
   o->remediation = DB2_VAULT_REMEDIATION_INTEGRITY;
   return DB2_VAULT_OPERATOR_INTEGRITY;
}

int db2_vault_operator_runtime_status(db2_vault_operator_runtime_t *r,
                                      db2_vault_provider_status_fn fn, void *ctx,
                                      db2_vault_operator_status_t *out)
{
   int64_t deadline;
   if (!r || !fn || !out || !r->mutex_initialized || deadline_new(&deadline) ||
       mutex_until(runtime_mutex(r), deadline))
      return DB2_VAULT_OPERATOR_UNAVAILABLE;
   memset(out, 0, sizeof(*out));
   for (int i = 0; i < 3; ++i)
   {
      db2_vault_operator_snapshot_t a, b;
      int rc = snapshot_locked(r, deadline, &a);
      if (rc)
      {
         pthread_mutex_unlock(runtime_mutex(r));
         return rc;
      }
      db2_vault_provider_status_t p = 0;
      if (fn(ctx, &p) || p < 1 || p > 4 || now_ms() < 0 || now_ms() >= deadline)
      {
         pthread_mutex_unlock(runtime_mutex(r));
         return DB2_VAULT_OPERATOR_UNAVAILABLE;
      }
      rc = snapshot_locked(r, deadline, &b);
      if (rc)
      {
         pthread_mutex_unlock(runtime_mutex(r));
         return rc;
      }
      if (!db2_vault_operator_snapshot_equal(&a, &b))
         continue;
      out->snapshot = b;
      out->provider = p;
      rc = classify(&b, p, out);
      pthread_mutex_unlock(runtime_mutex(r));
      return rc;
   }
   out->state = DB2_VAULT_STATE_INTEGRITY_FAILURE;
   out->remediation = DB2_VAULT_REMEDIATION_INTEGRITY;
   pthread_mutex_unlock(runtime_mutex(r));
   return DB2_VAULT_OPERATOR_INTEGRITY;
}
