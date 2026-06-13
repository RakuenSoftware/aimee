/* entity_registry.c: surrogate-id entity canonicalization (typed-fact §3 / P2a).
 * See entity_registry.h. */
#include "../headers/aimee.h"
#include "entity_registry.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define ER_ERRBUF 256

void entity_name_normalize(const char *in, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   size_t o = 0;
   int pending_space = 0;
   int started = 0;
   for (const char *p = in ? in : ""; *p && o + 1 < out_len; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isspace(c))
      {
         if (started)
            pending_space = 1; /* collapse; emit lazily before next real char */
         continue;
      }
      if (pending_space && o + 1 < out_len)
      {
         out[o++] = ' ';
         pending_space = 0;
      }
      if (o + 1 < out_len)
         out[o++] = (char)tolower(c);
      started = 1;
   }
   out[o] = '\0';
}

int64_t db2_entity_register(int kind, const char *status)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO entity_registry (kind, status) VALUES (?1, ?2)"
                        " RETURNING canonical_id",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", kind);
   aimee_pg_bind_text(st, "?2", (status && status[0]) ? status : ENTITY_STATUS_ACTIVE);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_entity_alias_bind(const char *name, int64_t canonical_id, int is_preferred)
{
   if (!name || !name[0] || canonical_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[256];
   entity_name_normalize(name, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   /* Reject a dangling alias: the target entity must exist (no FK on the sqlite
    * shim, so check explicitly — keeps the single-hop graph well-formed). */
   if (db2_entity_kind(canonical_id) < 0)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO entity_aliases (name, name_norm, canonical_id, is_preferred)"
                        " VALUES (?1, ?2, ?3, ?4) ON CONFLICT (name_norm) DO NOTHING",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_text(st, "?2", norm);
   aimee_pg_bind_int64(st, "?3", canonical_id);
   aimee_pg_bind_int(st, "?4", is_preferred ? 1 : 0);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int64_t db2_entity_resolve(const char *name)
{
   if (!name || !name[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[256];
   entity_name_normalize(name, norm, sizeof(norm));
   if (!norm[0])
      return 0;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT a.canonical_id, r.status, r.merged_into FROM entity_aliases a"
                        " JOIN entity_registry r ON a.canonical_id = r.canonical_id"
                        " WHERE a.name_norm = ?1 AND a.suppressed = 0 LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int64_t cid = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      cid = aimee_pg_column_int64(st, 0);
      const char *status = aimee_pg_column_text(st, 1);
      int64_t merged_into = aimee_pg_column_int64(st, 2);
      /* Follow a merged row one hop (aliases are single-hop; merges are too). */
      if (status && strcmp(status, ENTITY_STATUS_MERGED) == 0 && merged_into > 0)
         cid = merged_into;
   }
   aimee_pg_finalize(st);
   return cid;
}

int64_t db2_entity_register_named(const char *name, int kind)
{
   if (!name || !name[0])
      return -1;
   int64_t existing = db2_entity_resolve(name);
   if (existing > 0)
      return existing;
   if (existing < 0)
      return -1; /* DB error, not "absent" */
   int64_t cid = db2_entity_register(kind, ENTITY_STATUS_ACTIVE);
   if (cid <= 0)
      return -1;
   if (db2_entity_alias_bind(name, cid, 1) != 0)
      return -1;
   /* Race guard: a concurrent caller may have bound this name to a *different*
    * entity first (our bind then no-ops via ON CONFLICT). Re-resolve; if the name
    * now resolves elsewhere, our just-created row is an orphan — drop it (no alias
    * points to it) and return the winner, so callers never reference an orphan. */
   int64_t winner = db2_entity_resolve(name);
   if (winner > 0 && winner != cid)
   {
      void *conn = db2_conn();
      if (conn)
      {
         char err[ER_ERRBUF] = "";
         aimee_pg_stmt_t *d =
             aimee_pg_prepare(conn,
                              "DELETE FROM entity_registry WHERE canonical_id = ?1"
                              " AND canonical_id NOT IN (SELECT canonical_id FROM entity_aliases)",
                              err, sizeof(err));
         if (d)
         {
            aimee_pg_bind_int64(d, "?1", cid);
            (void)aimee_pg_step(d, err, sizeof(err));
            aimee_pg_finalize(d);
         }
      }
      return winner;
   }
   return cid;
}

int db2_entity_mark_merged(int64_t from_id, int64_t into_id)
{
   if (from_id <= 0 || into_id <= 0 || from_id == into_id)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE entity_registry SET status = 'merged', merged_into = ?2 WHERE canonical_id = ?1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", from_id);
   aimee_pg_bind_int64(st, "?2", into_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_entity_kind(int64_t canonical_id)
{
   if (canonical_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT kind FROM entity_registry WHERE canonical_id = ?1 LIMIT 1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", canonical_id);
   int kind = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      kind = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return kind;
}

int db2_entity_aliases_for(int64_t canonical_id, char (*out)[128], int max)
{
   if (canonical_id <= 0 || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ER_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT name FROM entity_aliases WHERE canonical_id = ?1 AND suppressed = 0"
                        " ORDER BY is_preferred DESC, id ASC LIMIT ?2",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", canonical_id);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *nm = aimee_pg_column_text(st, 0);
      snprintf(out[n], 128, "%s", nm ? nm : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
