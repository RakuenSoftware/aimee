/* db1/maintenance.h: DB1-file maintenance commands.
 *
 * Path-based wrappers for backup, integrity-check, and recovery against
 * the DB1 sqlite file. The implementations open/close their own
 * sqlite handle internally; callers never see one. */
#ifndef DEC_DB1_MAINTENANCE_H
#define DEC_DB1_MAINTENANCE_H 1

struct sqlite3;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Create a backup of the DB1 file at |db_path| using the SQLite
    * online backup API. If |out_path| is NULL, a timestamped filename
    * adjacent to |db_path| is used. Returns 0 on success, -1 on
    * failure. */
   int db1_backup(const char *db_path, const char *out_path);

   /* Run an integrity check on the DB1 file at |db_path|. If |full| is
    * non-zero, runs PRAGMA integrity_check; otherwise runs the cheaper
    * PRAGMA quick_check. Returns 0 if the file is intact, -1 on any
    * corruption / IO error. */
   int db1_check(const char *db_path, int full);

   /* Run PRAGMA quick_check on an already-open sqlite handle. Returns
    * 0 if ok.  Used by db1/diagnostics.c during recovery. */
   int db1_quick_check_sqlite(struct sqlite3 *db);

   /* Restore the DB1 file at |db_path| from the most recent valid
    * backup. If |force| is non-zero, will overwrite an existing file
    * even when no integrity issue is detected. Returns 0 on success,
    * -1 on failure. */
   int db1_recover(const char *db_path, int force);

   /* Returns the canonical DB1 sqlite file path (under XDG_DATA_HOME
    * or $HOME/.local/share). The returned pointer is owned by db1;
    * do not free. */
   const char *db1_default_path(void);

   /* Drop every cached prepared statement against the global DB1
    * connection. Used by long-running commands after schema-affecting
    * batch operations to prevent stale cursors from holding write
    * locks. Safe to call before db1_init / after db1_shutdown.
    *
    * Implementation lives in db.c; declared here so non-DB1 callers
    * (CLI command code) can reach it without including db.h's
    * sqlite3-bearing low-level surface. */
   void db1_stmt_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_MAINTENANCE_H */
