/* db2/trace_mining.c: trace-mining cursor — Postgres via libpq. */

#include "trace_mining.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>

int64_t db2_trace_mining_last_id(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COALESCE(MAX(last_trace_id), 0) FROM trace_mining_log", err, sizeof(err));
   if (!st)
      return 0;

   int64_t last_id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      last_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return last_id;
}

int db2_trace_mining_record(int64_t last_trace_id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "INSERT INTO trace_mining_log (last_trace_id, mined_at) VALUES (?1, pg_now_text())",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", last_trace_id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}
