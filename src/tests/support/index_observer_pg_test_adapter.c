/* Real libpq transport for the owned PostgreSQL observer qualification.
 * No SQL emulation, fallback or observer policy is implemented here. Production
 * uses db_postgres.c and the supervised PostgreSQL provider instead. */
#include "db_postgres.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
struct aimee_pg_stmt
{
   PGconn *connection;
   char *sql;
   char *values[64];
   int parameters, row;
   PGresult *result;
};
int aimee_pg_is_shim(void)
{
   return 0;
}
void *aimee_pg_open(const char *info, char *error, size_t capacity)
{
   PGconn *c = PQconnectdb(info);
   if (PQstatus(c) != CONNECTION_OK)
   {
      if (error && capacity)
         snprintf(error, capacity, "owned PostgreSQL connection unavailable");
      PQfinish(c);
      return NULL;
   }
   return c;
}
void aimee_pg_close(void *c)
{
   PQfinish(c);
}
void aimee_pg_discard(void *c)
{
   PQfinish(c);
}
int aimee_pg_in_transaction(void *c)
{
   PGTransactionStatusType s = PQtransactionStatus(c);
   return s == PQTRANS_INTRANS || s == PQTRANS_INERROR || s == PQTRANS_ACTIVE;
}
int aimee_pg_exec(void *c, const char *sql, char *error, size_t capacity)
{
   PGresult *r = PQexec(c, sql);
   int ok = r && (PQresultStatus(r) == PGRES_COMMAND_OK || PQresultStatus(r) == PGRES_TUPLES_OK);
   if (!ok && error && capacity)
      snprintf(error, capacity, "%s", r ? PQresultErrorMessage(r) : "query unavailable");
   PQclear(r);
   return ok ? 0 : -1;
}
aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *sql, char *error, size_t capacity)
{
   (void)error;
   (void)capacity;
   struct aimee_pg_stmt *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->connection = c;
   s->row = -1;
   s->sql = malloc(strlen(sql) * 2 + 1);
   if (!s->sql)
   {
      free(s);
      return NULL;
   }
   /* Retained source-owner statements use numbered ?N placeholders. Preserve
    * quoted strings/identifiers verbatim and let PostgreSQL parse the real SQL. */
   size_t out = 0;
   int quote = 0;
   for (size_t i = 0; sql[i]; i++)
   {
      if (quote)
      {
         s->sql[out++] = sql[i];
         if (sql[i] == quote)
         {
            if (sql[i + 1] == quote)
               s->sql[out++] = sql[++i];
            else
               quote = 0;
         }
         continue;
      }
      if (sql[i] == '\'' || sql[i] == '"')
      {
         quote = sql[i];
         s->sql[out++] = sql[i];
         continue;
      }
      if (sql[i] == '?' && isdigit((unsigned char)sql[i + 1]))
      {
         int n = 0;
         s->sql[out++] = '$';
         while (isdigit((unsigned char)sql[i + 1]))
         {
            i++;
            n = n * 10 + sql[i] - '0';
            s->sql[out++] = sql[i];
         }
         if (n < 1 || n > 64)
         {
            aimee_pg_finalize(s);
            return NULL;
         }
         if (n > s->parameters)
            s->parameters = n;
      }
      else
         s->sql[out++] = sql[i];
   }
   s->sql[out] = 0;
   return s;
}
static int bind_text(aimee_pg_stmt_t *s, const char *name, const char *value)
{
   if (!s || !name || name[0] != '?')
      return -1;
   int n = atoi(name + 1);
   if (n < 1 || n > 64)
      return -1;
   free(s->values[n - 1]);
   s->values[n - 1] = value ? strdup(value) : NULL;
   return !value || s->values[n - 1] ? 0 : -1;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *name, const char *value)
{
   return bind_text(s, name, value);
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *name, int64_t value)
{
   char text[32];
   snprintf(text, sizeof(text), "%lld", (long long)value);
   return bind_text(s, name, text);
}
int aimee_pg_bind_int(aimee_pg_stmt_t *s, const char *name, int value)
{
   return aimee_pg_bind_int64(s, name, value);
}
int aimee_pg_bind_double(aimee_pg_stmt_t *s, const char *name, double value)
{
   char text[64];
   snprintf(text, sizeof(text), "%.17g", value);
   return bind_text(s, name, text);
}
int aimee_pg_bind_null(aimee_pg_stmt_t *s, const char *name)
{
   return bind_text(s, name, NULL);
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *error, size_t capacity)
{
   if (!s)
      return AIMEE_PG_ERR;
   if (!s->result)
   {
      s->result = PQexecParams(s->connection, s->sql, s->parameters, NULL,
                               (const char *const *)s->values, NULL, NULL, 0);
      if (!s->result || (PQresultStatus(s->result) != PGRES_COMMAND_OK &&
                         PQresultStatus(s->result) != PGRES_TUPLES_OK))
      {
         if (error && capacity)
            snprintf(error, capacity, "%s",
                     s->result ? PQresultErrorMessage(s->result) : "query unavailable");
         return AIMEE_PG_ERR;
      }
   }
   if (PQresultStatus(s->result) == PGRES_COMMAND_OK)
      return AIMEE_PG_DONE;
   return ++s->row < PQntuples(s->result) ? AIMEE_PG_ROW : AIMEE_PG_DONE;
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   if (!s)
      return;
   PQclear(s->result);
   for (int i = 0; i < 64; i++)
      free(s->values[i]);
   free(s->sql);
   free(s);
}
int aimee_pg_reset(aimee_pg_stmt_t *s)
{
   if (!s)
      return -1;
   PQclear(s->result);
   s->result = NULL;
   s->row = -1;
   return 0;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int column)
{
   if (!s || !s->result || s->row < 0 || column < 0 || column >= PQnfields(s->result) ||
       PQgetisnull(s->result, s->row, column))
      return NULL;
   return PQgetvalue(s->result, s->row, column);
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int column)
{
   const char *v = aimee_pg_column_text(s, column);
   return v ? strtoll(v, NULL, 10) : 0;
}
int aimee_pg_column_int(aimee_pg_stmt_t *s, int column)
{
   return (int)aimee_pg_column_int64(s, column);
}
double aimee_pg_column_double(aimee_pg_stmt_t *s, int column)
{
   const char *v = aimee_pg_column_text(s, column);
   return v ? strtod(v, NULL) : 0;
}
int aimee_pg_column_type(aimee_pg_stmt_t *s, int column)
{
   if (!aimee_pg_column_text(s, column))
      return AIMEE_PG_VALUE_NULL;
   Oid type = PQftype(s->result, column);
   return type == 20 || type == 21 || type == 23       ? AIMEE_PG_VALUE_INTEGER
          : type == 700 || type == 701 || type == 1700 ? AIMEE_PG_VALUE_FLOAT
                                                       : AIMEE_PG_VALUE_TEXT;
}
int aimee_pg_stmt_changes(aimee_pg_stmt_t *s)
{
   const char *v = s && s->result ? PQcmdTuples(s->result) : NULL;
   return v && v[0] ? atoi(v) : 0;
}
const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *s)
{
   return s && s->result ? PQresultErrorField(s->result, PG_DIAG_SQLSTATE) : NULL;
}
void *aimee_pg_open_configured(char *error, size_t capacity)
{
   return aimee_pg_open(getenv("AIMEE_INDEX_OBSERVER_TEST_DSN"), error, capacity);
}
void *aimee_pg_open_runtime(char *error, size_t capacity)
{
   return aimee_pg_open_configured(error, capacity);
}
void *aimee_pg_open_migration(char *error, size_t capacity)
{
   return aimee_pg_open_configured(error, capacity);
}

int aimee_pg_column_is_null(aimee_pg_stmt_t *s, int column)
{
   return !aimee_pg_column_text(s, column);
}
