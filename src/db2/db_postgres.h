#ifndef DEC_DB_POSTGRES_H
#define DEC_DB_POSTGRES_H 1

/* libpq-backed helpers used by DB2. The PGconn / PGresult types stay
 * encapsulated here so callers don't pull in libpq-fe.h. */

#include <stddef.h>
#include <stdint.h>

typedef enum
{
   AIMEE_PG_DONE = 0,
   AIMEE_PG_ROW = 1,
   AIMEE_PG_ERR = -1,
} aimee_pg_step_t;

typedef enum
{
   AIMEE_PG_VALUE_NULL = 0,
   AIMEE_PG_VALUE_INTEGER = 1,
   AIMEE_PG_VALUE_FLOAT = 2,
   AIMEE_PG_VALUE_TEXT = 3,
   AIMEE_PG_VALUE_BLOB = 4,
} aimee_pg_value_type_t;

/* Rewrite :name placeholders in SQL to libpq $N form, building a
 * positional-index → name table along the way. Respects single-quoted
 * string literals, double-quoted identifiers, SQL line comments,
 * SQL block comments, and the :: cast operator.
 *
 * Repeated :name references reuse the same $N.
 *
 * On success: writes the rewritten SQL to *out_sql (malloc'd), the
 * name array to *out_names (malloc'd, count = *out_count), and
 * returns 0. On failure: writes an explanation to errbuf and returns
 * -1. The caller is responsible for freeing with aimee_pg_free_names. */
int aimee_pg_rewrite_params(const char *sql_in, char **out_sql, char ***out_names, int *out_count,
                            char *errbuf, size_t errlen);
void aimee_pg_free_names(char **names, int count);

typedef struct aimee_pg_stmt aimee_pg_stmt_t;

void *aimee_pg_open(const char *conninfo, char *errbuf, size_t errlen);
void aimee_pg_close(void *pg_conn);

/* Returns 1 when the linked aimee_pg_* implementation is the test shim, 0 when
 * it is the real libpq implementation. Lets shared code paths opt into a
 * "shim" db2_init only in test builds, without dragging libpq into a
 * production DB2 connect attempt. */
int aimee_pg_is_shim(void);
int aimee_pg_exec(void *pg_conn, const char *sql, char *errbuf, size_t errlen);
int aimee_pg_exec_with_changes(void *pg_conn, const char *sql, char *errbuf, size_t errlen,
                               int *affected_out);
/* 1 if the connection currently has an open (or failed-open) transaction, else 0.
 * Lets callers skip a no-op ROLLBACK (which makes postgres log "no transaction in
 * progress" on every clean pool return). */
int aimee_pg_in_transaction(void *pg_conn);
int aimee_pg_stmt_changes(aimee_pg_stmt_t *stmt);
int aimee_pg_ping(void *pg_conn, char *errbuf, size_t errlen);

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen);
void aimee_pg_finalize(aimee_pg_stmt_t *stmt);
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *errbuf, size_t errlen);
int aimee_pg_reset(aimee_pg_stmt_t *stmt);

int aimee_pg_bind_int(aimee_pg_stmt_t *stmt, const char *name, int value);
int aimee_pg_bind_int64(aimee_pg_stmt_t *stmt, const char *name, int64_t value);
int aimee_pg_bind_double(aimee_pg_stmt_t *stmt, const char *name, double value);
int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value);
int aimee_pg_bind_blob(aimee_pg_stmt_t *stmt, const char *name, const void *value, int len);
int aimee_pg_bind_null(aimee_pg_stmt_t *stmt, const char *name);

int aimee_pg_column_count(aimee_pg_stmt_t *stmt);
const char *aimee_pg_column_name(aimee_pg_stmt_t *stmt, int col);
int aimee_pg_column_type(aimee_pg_stmt_t *stmt, int col);
const void *aimee_pg_column_blob(aimee_pg_stmt_t *stmt, int col);
int aimee_pg_column_bytes(aimee_pg_stmt_t *stmt, int col);
int aimee_pg_column_is_null(aimee_pg_stmt_t *stmt, int col);
int aimee_pg_column_int(aimee_pg_stmt_t *stmt, int col);
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *stmt, int col);
double aimee_pg_column_double(aimee_pg_stmt_t *stmt, int col);
const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col);

#endif /* DEC_DB_POSTGRES_H */
