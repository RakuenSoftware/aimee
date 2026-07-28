/* db2/memory_scope_query.c: read-side scope-tag probes against memory_scopes
 * and the legacy memory_workspaces table. Postgres via libpq. */

#include "../headers/aimee.h" /* memory_t */
#include "memory_query.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MSQ_ERRBUF 256

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
}

int db2_memory_source_context_link(int64_t memory_id, const char *repository_key,
                                   const char *source_ref, const char *source_commit,
                                   int64_t index_generation_id, const char *evidence_role)
{
   if (memory_id <= 0 || !repository_key || !repository_key[0] || !source_ref ||
       !source_ref[0] || !source_commit || !source_commit[0] || index_generation_id <= 0)
      return -1;
   const char *role = evidence_role && evidence_role[0] ? evidence_role : "code_derived";
   if (strcmp(role, "code_derived") != 0 && strcmp(role, "document_derived") != 0 &&
       strcmp(role, "mixed") != 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "INSERT INTO memory_source_contexts"
       " (memory_id, repository_key, source_ref, source_commit, index_generation_id,"
       "  evidence_role) VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT DO NOTHING";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", repository_key);
   aimee_pg_bind_text(st, "?3", source_ref);
   aimee_pg_bind_text(st, "?4", source_commit);
   aimee_pg_bind_int64(st, "?5", index_generation_id);
   aimee_pg_bind_text(st, "?6", role);
   int rc = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_source_context_visible(int64_t memory_id, const char *repository_key,
                                      const char *source_ref, const char *source_commit,
                                      int64_t index_generation_id)
{
   if (memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT CASE WHEN EXISTS ("
       "  SELECT 1 FROM memory_source_contexts WHERE memory_id = ?1"
       ") THEN CASE WHEN EXISTS ("
       "  SELECT 1 FROM memory_source_contexts"
       "   WHERE memory_id = ?1 AND repository_key = ?2 AND source_ref = ?3"
       "     AND source_commit = ?4 AND index_generation_id = ?5"
       ") THEN 1 ELSE 0 END ELSE 1 END";
   char err[MSQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", repository_key ? repository_key : "");
   aimee_pg_bind_text(st, "?3", source_ref ? source_ref : "");
   aimee_pg_bind_text(st, "?4", source_commit ? source_commit : "");
   aimee_pg_bind_int64(st, "?5", index_generation_id);
   int visible = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      visible = aimee_pg_column_int(st, 0) ? 1 : 0;
   aimee_pg_finalize(st);
   return visible;
}

void db2_memory_workspace_tag_insert(int64_t memory_id, const char *workspace)
{
   if (memory_id <= 0 || !workspace || !workspace[0])
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
