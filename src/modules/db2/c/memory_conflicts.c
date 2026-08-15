/* db2/memory_conflicts.c: SQL primitives for memory_conflicts and
 * contradiction_log — Postgres via libpq. */

#include "../headers/aimee.h" /* conflict_t */
#include "memory_conflicts.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MC_ERRBUF 256

int db2_memory_conflict_record(int64_t mem_a, int64_t mem_b)
{
   if (mem_a <= 0 || mem_b <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   db2_now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO memory_conflicts (memory_a, memory_b, detected_at,"
                            " resolved) VALUES (?1, ?2, ?3, 0)";
   char err[MC_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", mem_a);
   aimee_pg_bind_int64(st, "?2", mem_b);
   aimee_pg_bind_text(st, "?3", ts);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

void db2_memory_contradiction_log(int64_t mem_a, int64_t mem_b, const char *resolution,
                                  const char *details)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   char ts[32];
   db2_now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO contradiction_log"
                            " (detected_at, memory_a_id, memory_b_id, resolution, details)"
                            " VALUES (?1, ?2, ?3, ?4, ?5)";
   char err[MC_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_int64(st, "?2", mem_a);
   aimee_pg_bind_int64(st, "?3", mem_b);
   aimee_pg_bind_text(st, "?4", resolution ? resolution : "pending");
   if (details)
      aimee_pg_bind_text(st, "?5", details);
   else
      aimee_pg_bind_null(st, "?5");
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_memory_conflict_list(conflict_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id, memory_a, memory_b, detected_at, resolved, resolution"
                            "  FROM memory_conflicts WHERE resolved = 0"
                            " ORDER BY detected_at DESC";
   char err[MC_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id = aimee_pg_column_int64(st, 0);
      out[n].memory_a = aimee_pg_column_int64(st, 1);
      out[n].memory_b = aimee_pg_column_int64(st, 2);
      const char *dat = aimee_pg_column_text(st, 3);
      snprintf(out[n].detected_at, sizeof(out[n].detected_at), "%s", dat ? dat : "");
      out[n].resolved = aimee_pg_column_int(st, 4);
      const char *res = aimee_pg_column_text(st, 5);
      snprintf(out[n].resolution, sizeof(out[n].resolution), "%s", res ? res : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_conflict_get_pair(int64_t conflict_id, int64_t *mem_a, int64_t *mem_b)
{
   if (mem_a)
      *mem_a = 0;
   if (mem_b)
      *mem_b = 0;
   if (conflict_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT memory_a, memory_b FROM memory_conflicts WHERE id = ?1";
   char err[MC_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", conflict_id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (mem_a)
         *mem_a = aimee_pg_column_int64(st, 0);
      if (mem_b)
         *mem_b = aimee_pg_column_int64(st, 1);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_conflict_resolve(int64_t conflict_id, const char *resolution)
{
   if (conflict_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "UPDATE memory_conflicts SET resolved = 1, resolution = ?1"
                            " WHERE id = ?2";
   char err[MC_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", resolution ? resolution : "");
   aimee_pg_bind_int64(st, "?2", conflict_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : -1;
   aimee_pg_finalize(st);
   return changes;
}
