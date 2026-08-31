/* db2/memory_scope_query.c: read-side scope-tag probes against memory_scopes
 * and the legacy memory_workspaces table. Postgres via libpq. */

#include "../headers/aimee.h" /* memory_t */
#include "memory_query.h"
#include "memory_scope_query.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MSQ_ERRBUF 256

/* Ids per IN(...) statement. Bounds the placeholder buffer; a longer candidate
 * list is split across statements rather than truncated. */
#define MSQ_RANK_BATCH_CHUNK 128

static __thread db2_memory_scope_context_t s_memory_scope_context;

/* Keep PostgreSQL RLS in lockstep with the in-process filter.  These values are
 * derived by the trusted request boundary, never copied from an assertion or
 * memory payload. */
static void memory_scope_sync_pg(void)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT set_config('aimee.memory_workspace',?1,false),"
                                          " set_config('aimee.memory_project',?2,false),"
                                          " set_config('aimee.memory_scope_all',?3,false),"
                                          " set_config('aimee.memory_scope_type',?4,false),"
                                          " set_config('aimee.memory_scope_value',?5,false)",
                                          err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", s_memory_scope_context.workspace);
   aimee_pg_bind_text(st, "?2", s_memory_scope_context.project);
   aimee_pg_bind_text(st, "?3", s_memory_scope_context.include_all ? "1" : "0");
   aimee_pg_bind_text(st, "?4", s_memory_scope_context.scope_type);
   aimee_pg_bind_text(st, "?5", s_memory_scope_context.scope_value);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_scope_context_set(const char *workspace, const char *project, int include_all)
{
   db2_memory_scope_context_set_exact(workspace, project, NULL, NULL, include_all);
}

void db2_memory_scope_context_set_exact(const char *workspace, const char *project,
                                        const char *scope_type, const char *scope_value,
                                        int include_all)
{
   memset(&s_memory_scope_context, 0, sizeof(s_memory_scope_context));
   s_memory_scope_context.active = 1;
   s_memory_scope_context.include_all = include_all ? 1 : 0;
   snprintf(s_memory_scope_context.workspace, sizeof(s_memory_scope_context.workspace), "%s",
            workspace ? workspace : "");
   snprintf(s_memory_scope_context.project, sizeof(s_memory_scope_context.project), "%s",
            project ? project : "");
   snprintf(s_memory_scope_context.scope_type, sizeof(s_memory_scope_context.scope_type), "%s",
            scope_type ? scope_type : "");
   snprintf(s_memory_scope_context.scope_value, sizeof(s_memory_scope_context.scope_value), "%s",
            scope_value ? scope_value : "");
   memory_scope_sync_pg();
}

void db2_memory_scope_context_restore(const db2_memory_scope_context_t *context)
{
   if (context)
      s_memory_scope_context = *context;
   else
      memset(&s_memory_scope_context, 0, sizeof(s_memory_scope_context));
   memory_scope_sync_pg();
}

void db2_memory_scope_context_clear(void)
{
   memset(&s_memory_scope_context, 0, sizeof(s_memory_scope_context));
   memory_scope_sync_pg();
}

void db2_memory_scope_context_get(db2_memory_scope_context_t *out)
{
   if (out)
      *out = s_memory_scope_context;
}

/* The visibility rank, expressed once. The single-id and batch readers below
 * both call this: two copies of a four-level visibility ladder is how a store
 * starts disagreeing with itself about who can see what. */
static int msq_rank_for(const char *type, const char *value)
{
   if (!type || !value)
      return 0;
   if (s_memory_scope_context.scope_type[0] && s_memory_scope_context.scope_value[0] &&
       strcmp(type, s_memory_scope_context.scope_type) == 0 &&
       strcmp(value, s_memory_scope_context.scope_value) == 0)
      return 4;
   if (strcmp(type, "project") == 0 && s_memory_scope_context.project[0] &&
       strcmp(value, s_memory_scope_context.project) == 0)
      return 3;
   if (strcmp(type, "workspace") == 0 && s_memory_scope_context.workspace[0] &&
       strcmp(value, s_memory_scope_context.workspace) == 0)
      return 2;
   if ((strcmp(type, "global") == 0 && strcmp(value, "_global") == 0) ||
       (strcmp(type, "workspace") == 0 && strcmp(value, "_shared") == 0))
      return 1;
   return 0;
}

int db2_memory_scope_context_rank(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT scope_type,scope_value FROM memories WHERE id=?1", err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int rank = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      rank = msq_rank_for(aimee_pg_column_text(st, 0), aimee_pg_column_text(st, 1));
   aimee_pg_finalize(st);
   return rank;
}

/* Batch form of the above: ONE statement for the whole candidate set instead of
 * one per candidate. The ranking is thread-local scope context plus two strings
 * per row, so the per-id work was never the cost -- the round trip was, and a
 * recall ranks every candidate it is about to sort.
 *
 * Chunked so an arbitrarily long candidate list cannot overflow the placeholder
 * buffer. Ids absent from `memories` keep rank 0, matching the single-id reader
 * when its query returns no row. */
int db2_memory_scope_context_rank_batch(const int64_t *ids, int n, int *out_ranks)
{
   if (!ids || n <= 0 || !out_ranks)
      return 0;
   for (int i = 0; i < n; i++)
      out_ranks[i] = 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   int ranked = 0;
   for (int base = 0; base < n; base += MSQ_RANK_BATCH_CHUNK)
   {
      int chunk = n - base < MSQ_RANK_BATCH_CHUNK ? n - base : MSQ_RANK_BATCH_CHUNK;
      char placeholders[MSQ_RANK_BATCH_CHUNK * 8];
      int pos = 0;
      for (int i = 0; i < chunk; i++)
         pos += snprintf(placeholders + pos, sizeof(placeholders) - (size_t)pos,
                         i == 0 ? "?%d" : ",?%d", i + 1);
      char sql[sizeof(placeholders) + 128];
      snprintf(sql, sizeof(sql), "SELECT id,scope_type,scope_value FROM memories WHERE id IN (%s)",
               placeholders);

      char err[MSQ_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return ranked;
      for (int i = 0; i < chunk; i++)
      {
         char name[16];
         snprintf(name, sizeof(name), "?%d", i + 1);
         aimee_pg_bind_int64(st, name, ids[base + i]);
      }
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         int64_t row_id = aimee_pg_column_int64(st, 0);
         int rank = msq_rank_for(aimee_pg_column_text(st, 1), aimee_pg_column_text(st, 2));
         /* A candidate list may repeat an id; rank every position holding it. */
         for (int i = 0; i < chunk; i++)
            if (ids[base + i] == row_id)
            {
               out_ranks[base + i] = rank;
               ranked++;
            }
      }
      aimee_pg_finalize(st);
   }
   return ranked;
}

int db2_memory_scope_context_allows(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   if (!s_memory_scope_context.active || s_memory_scope_context.include_all)
      return 1;
   return db2_memory_scope_context_rank(memory_id) > 0;
}

void db2_memory_scope_bind_current(aimee_pg_stmt_t *st)
{
   if (!st)
      return;
   aimee_pg_bind_int(st, "?101", s_memory_scope_context.active ? 1 : 0);
   aimee_pg_bind_int(st, "?102", s_memory_scope_context.include_all ? 1 : 0);
   aimee_pg_bind_text(st, "?103", s_memory_scope_context.workspace);
   aimee_pg_bind_text(st, "?104", s_memory_scope_context.project);
   aimee_pg_bind_text(st, "?105", s_memory_scope_context.scope_type);
   aimee_pg_bind_text(st, "?106", s_memory_scope_context.scope_value);
}

int db2_memory_scope_matches(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   if (!scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT 1 FROM memory_scopes WHERE memory_id = ?1 AND scope_type = ?2 AND scope_value = ?3"
       " LIMIT 1";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", scope_type);
   aimee_pg_bind_text(st, "?3", scope_value);
   int hit = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_workspace_matches(int64_t memory_id, const char *workspace)
{
   if (!workspace || !workspace[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT 1 FROM memory_workspaces WHERE memory_id = ?1 AND workspace = ?2 LIMIT 1";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", workspace);
   int hit = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_has_scope_type(int64_t memory_id, const char *scope_type)
{
   if (memory_id <= 0 || !scope_type || !scope_type[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT 1 FROM memory_scopes WHERE memory_id = ?1 AND scope_type = ?2 LIMIT 1";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", scope_type);
   int hit = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_has_any_workspace_tag(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT 1 FROM memory_workspaces WHERE memory_id = ?1 LIMIT 1";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int hit = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_scopes_list(int64_t memory_id, db2_memory_scope_tag_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT scope_type, scope_value FROM memory_scopes WHERE memory_id = ?1"
                            " ORDER BY CASE scope_type"
                            "            WHEN 'project' THEN 0"
                            "            WHEN 'workspace' THEN 1"
                            "            WHEN 'global' THEN 2"
                            "            ELSE 3 END, scope_value ASC";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *t = aimee_pg_column_text(st, 0);
      const char *v = aimee_pg_column_text(st, 1);
      snprintf(out[n].type, sizeof(out[n].type), "%s", t ? t : "");
      snprintf(out[n].value, sizeof(out[n].value), "%s", v ? v : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_scope_tag_insert(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   if (memory_id <= 0 || !scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return;
   /* A scope tag changes ownership.  Keep the application-side authorization
    * check even when the development connection is an RLS-bypassing owner. */
   if (!db2_memory_scope_context_allows(memory_id))
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   static const char *sql = "INSERT INTO memory_scopes (memory_id, scope_type, scope_value)"
                            " VALUES (?1, ?2, ?3) ON CONFLICT DO NOTHING";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", scope_type);
   aimee_pg_bind_text(st, "?3", scope_value);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);

   /* The primary authorization identity is denormalized onto memories so an
    * RLS policy can decide visibility without trusting a neighbouring table. */
   st = aimee_pg_prepare(conn,
                         "UPDATE memories SET scope_type=?2,scope_value=?3,updated_at=pg_now_text()"
                         " WHERE id=?1",
                         err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", scope_type);
   aimee_pg_bind_text(st, "?3", scope_value);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_workspace_tag_insert(int64_t memory_id, const char *workspace)
{
   if (memory_id <= 0 || !workspace || !workspace[0])
      return;
   if (!db2_memory_scope_context_allows(memory_id))
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   static const char *sql = "INSERT INTO memory_workspaces (memory_id, workspace)"
                            " VALUES (?1, ?2) ON CONFLICT DO NOTHING";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", workspace);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_memory_scope_replace(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   if (memory_id <= 0 || !scope_type || !scope_value || !scope_value[0] ||
       (strcmp(scope_type, "project") != 0 && strcmp(scope_type, "workspace") != 0 &&
        strcmp(scope_type, "global") != 0))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[MSQ_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   static const char *const delete_sql[] = {
       "DELETE FROM memory_scopes WHERE memory_id=?1",
       "DELETE FROM memory_workspaces WHERE memory_id=?1",
   };
   for (size_t i = 0; i < sizeof(delete_sql) / sizeof(delete_sql[0]); i++)
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, delete_sql[i], err, sizeof(err));
      if (!st)
         goto rollback;
      aimee_pg_bind_int64(st, "?1", memory_id);
      int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(st);
      if (!ok)
         goto rollback;
   }

   aimee_pg_stmt_t *insert = aimee_pg_prepare(
       conn, "INSERT INTO memory_scopes(memory_id,scope_type,scope_value) VALUES(?1,?2,?3)", err,
       sizeof(err));
   if (!insert)
      goto rollback;
   aimee_pg_bind_int64(insert, "?1", memory_id);
   aimee_pg_bind_text(insert, "?2", scope_type);
   aimee_pg_bind_text(insert, "?3", scope_value);
   int inserted = aimee_pg_step(insert, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(insert);
   if (!inserted)
      goto rollback;

   if (strcmp(scope_type, "workspace") == 0)
   {
      insert =
          aimee_pg_prepare(conn, "INSERT INTO memory_workspaces(memory_id,workspace) VALUES(?1,?2)",
                           err, sizeof(err));
      if (!insert)
         goto rollback;
      aimee_pg_bind_int64(insert, "?1", memory_id);
      aimee_pg_bind_text(insert, "?2", scope_value);
      inserted = aimee_pg_step(insert, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(insert);
      if (!inserted)
         goto rollback;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) == 0)
      return 0;

rollback:
   (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return -1;
}
