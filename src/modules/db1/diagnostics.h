/* db1/diagnostics.h: read-only DB1 diagnostic helpers used by
 * `aimee status` and `aimee doctor`. Both verbs need to inspect the
 * sqlite file on disk before db1_init has been called (or even
 * succeeded), so the helpers open the file by path rather than going
 * through the live db1 connection. */
#ifndef DEC_DB1_DIAGNOSTICS_H
#define DEC_DB1_DIAGNOSTICS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int opened;         /* 1 if sqlite3_open succeeded */
      int schema_version; /* PRAGMA user_version on success, 0 otherwise */
      int fts5_ok;        /* 1 ok, 0 corrupted, -1 not checked */
      long size_bytes;    /* file size or -1 on stat failure */
   } db1_diag_t;

   /* Inspect the DB1 file at |path|. If |check_fts| is non-zero,
    * additionally runs the memories_fts integrity-check pragma and
    * sets fts5_ok accordingly (this requires read-write access). The
    * connection used for the inspection is opened and closed
    * internally — callers don't see a sqlite handle. */
   void db1_diag_inspect(const char *path, int check_fts, db1_diag_t *out);

   /* Validate the DB1 file at |path|: open, run PRAGMA quick_check,
    * close. If quick_check fails, attempt db1_recover(path, 0) and
    * re-open to confirm the file is now usable. Returns:
    *    0 — file is intact, nothing done
    *    1 — file was corrupted but recovered successfully
    *   -1 — file cannot be opened, or quick_check failed and recovery
    *        also failed
    *   -2 — file cannot be opened at all
    * Used by session-startup paths that need to gate further work on
    * DB1 being readable. */
   int db1_diag_quick_check_recover(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DIAGNOSTICS_H */
