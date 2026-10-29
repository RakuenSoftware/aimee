/* vector_index_ops.c: KB_STORE-side bookkeeping for pgvector write status.
 *
 * Postgres via libpq. The pgvector transport lives in src/modules/kb/c/pgvec_*;
 * this file only persists shared retry/status metadata for those writes. */

#include "vector_index_ops.h"

#include "aimee.h"
#include "kb_store_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void kb_store_vector_index_op_record(int64_t point_id, const char *collection, int64_t memory_id,
                                     int ok, const char *error_msg)
{
   void *conn = kb_store_conn();
   if (!conn)
      return;

   static const char *sql =
       "INSERT INTO vector_index_ops"
       "  (point_id, collection, memory_id, status, attempts, last_error, indexed_at, updated_at)"
       " VALUES (?1, ?2, ?3, ?4, 1, ?5, ?6, pg_now_text())"
       " ON CONFLICT(point_id) DO UPDATE SET"
       "  collection = excluded.collection,"
       "  status     = excluded.status,"
       "  attempts   = vector_index_ops.attempts + 1,"
       "  last_error = excluded.last_error,"
       "  indexed_at = excluded.indexed_at,"
       "  updated_at = pg_now_text()";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_int64(st, "?1", point_id);
   aimee_pg_bind_text(st, "?2", collection ? collection : "");
   if (memory_id > 0)
      aimee_pg_bind_int64(st, "?3", memory_id);
   else
      aimee_pg_bind_null(st, "?3");
   aimee_pg_bind_text(st, "?4", ok ? "ok" : "failed");
   aimee_pg_bind_text(st, "?5", (error_msg && !ok) ? error_msg : "");
   if (ok)
      aimee_pg_bind_text(st, "?6", ts);
   else
      aimee_pg_bind_null(st, "?6");
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void kb_store_vector_index_op_remove(int64_t point_id)
{
   void *conn = kb_store_conn();
   if (!conn)
      return;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM vector_index_ops WHERE point_id = ?1", err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", point_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}
