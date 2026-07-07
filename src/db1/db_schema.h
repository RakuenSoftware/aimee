#ifndef DEC_DB_SCHEMA_H
#define DEC_DB_SCHEMA_H 1

/* DB1 (SQLite) idempotent schema bootstrap. Single CREATE-IF-NOT-EXISTS
 * pass; safe to call on an already-initialized database. The DB2 half
 * lives in db2/db_schema.h. See docs/STORAGE_TIERS.md. */

#include <stddef.h>

struct sqlite3;

/* Apply the DB1 SQLite schema to an open connection.
 * Returns 0 on success, -1 on failure (writes to errbuf/errlen). */
int db1_apply_schema_sqlite(struct sqlite3 *db, char *errbuf, size_t errlen);

/* Reconcile the live database's column set against the canonical schema.
 * For each table in the canonical schema, issues ALTER TABLE ADD COLUMN for
 * any column that is missing on the live database.  Idempotent and
 * non-destructive — it never removes or renames columns. */
void db1_reconcile_columns(struct sqlite3 *db);

#endif /* DEC_DB_SCHEMA_H */
