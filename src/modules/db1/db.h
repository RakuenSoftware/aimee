#ifndef DEC_DB_H
#define DEC_DB_H 1

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sqlite3.h>

/* Pragma profile mode: CLI is short-lived (FULL sync), server is long-lived (NORMAL sync). */
typedef enum
{
   DB_MODE_CLI = 0,
   DB_MODE_SERVER
} db_mode_t;

/* Apply the appropriate SQLite pragma profile for the given mode. */
void db1_apply_pragmas(sqlite3 *db, db_mode_t mode);

/* Prepared statement cache: get or create. */
sqlite3_stmt *db1_prepare(sqlite3 *db, const char *sql);

/* Close all cached statements. Call before sqlite3_close. */
void db1_stmt_cache_clear(void);

/* Close cached statements for a specific db connection only. */
void db1_stmt_cache_clear_for_sqlite(sqlite3 *db);

/* Check FTS5 availability. */
int db1_fts5_available(sqlite3 *db);

/* Step a statement and log on failure. Use for fire-and-forget writes
 * (provenance, metrics, health) where silent failure causes data loss. */
static inline void db_step_log_sqlite(sqlite3_stmt *stmt, const char *label)
{
   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE && rc != SQLITE_ROW)
      fprintf(stderr, "aimee: sqlite3_step failed in %s: %s\n", label,
              sqlite3_errmsg(sqlite3_db_handle(stmt)));
}
#define DB_STEP_LOG(stmt, label) db_step_log_sqlite((stmt), (label))

#endif /* DEC_DB_H */
