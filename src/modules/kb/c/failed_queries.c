/* db2/failed_queries.c: failed-query counter — Postgres via libpq. */

#include "failed_queries.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>

int db2_failed_query_bump(const char *query_norm)
{
   if (!query_norm || !*query_norm)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *upsert =
       aimee_pg_prepare(conn,
                        "INSERT INTO failed_queries(query_norm, failure_count, last_failed_at)"
                        " VALUES (?1, 1, pg_now_text())"
                        " ON CONFLICT(query_norm) DO UPDATE SET"
                        "   failure_count = failed_queries.failure_count + 1,"
                        "   last_failed_at = pg_now_text()",
                        err, sizeof(err));
   if (!upsert)
      return 0;
   aimee_pg_bind_text(upsert, "?1", query_norm);
   aimee_pg_step_t rc = aimee_pg_step(upsert, err, sizeof(err));
   aimee_pg_finalize(upsert);
   if (rc != AIMEE_PG_DONE)
      return 0;

   aimee_pg_stmt_t *read = aimee_pg_prepare(
       conn, "SELECT failure_count FROM failed_queries WHERE query_norm = ?1", err, sizeof(err));
   if (!read)
      return 0;
   aimee_pg_bind_text(read, "?1", query_norm);
   int count = 0;
   if (aimee_pg_step(read, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(read, 0);
   aimee_pg_finalize(read);
   return count;
}
