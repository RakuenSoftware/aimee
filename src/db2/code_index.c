/* db2/code_index.c: code-index primitives — Postgres via libpq. */

#include "code_index.h"
#include "../headers/aimee.h"      /* MAX_PATH_LEN, now_utc */
#include "../headers/code_match.h" /* code_match_line (P1b span enrichment) */
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CIDX_ERRBUF 256

static int64_t code_index_resolve_project(void *conn, const char *name)
{
   if (!conn || !name)
      return -1;
   static const char *sql = "SELECT id FROM projects WHERE name = ?1";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int64_t code_index_resolve_file(void *conn, int64_t project_id, const char *rel_path)
{
   if (!conn || !rel_path)
      return -1;
   static const char *sql = "SELECT id FROM files WHERE project_id = ?1 AND path = ?2";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", rel_path);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_code_index_project_count(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT COUNT(*) FROM projects";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_code_index_project_last_scan(char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT MAX(scanned_at) FROM projects";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ts = aimee_pg_column_text(st, 0);
      if (ts)
         snprintf(out, cap, "%s", ts);
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_code_index_project_list(project_info_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT name, root, scanned_at FROM projects"
                            " WHERE root NOT LIKE '%/.%'"
                            " ORDER BY name";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *n = aimee_pg_column_text(st, 0);
      const char *r = aimee_pg_column_text(st, 1);
      const char *s = aimee_pg_column_text(st, 2);
      snprintf(out[count].name, sizeof(out[count].name), "%s", n ? n : "");
      snprintf(out[count].root, sizeof(out[count].root), "%s", r ? r : "");
      snprintf(out[count].scanned_at, sizeof(out[count].scanned_at), "%s", s ? s : "");
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

/* Escape a user identifier for LIKE, leaving room at both ends for
 * caller-supplied wildcards. Produces "<lead>escaped(id)<trail>", where
 * <lead> and <trail> are either '%' (wildcard) or empty (anchor). The
 * companion SQL must use ESCAPE '\\'. */
static void cidx_make_like_pattern(const char *id, int leading_pct, int trailing_pct, char *buf,
                                   size_t buflen)
{
   size_t j = 0;
   if (leading_pct && j + 1 < buflen)
      buf[j++] = '%';
   for (size_t i = 0; id[i] && j + 3 < buflen; i++)
   {
      if (id[i] == '%' || id[i] == '_' || id[i] == '\\')
         buf[j++] = '\\';
      buf[j++] = (unsigned char)id[i];
   }
   if (trailing_pct && j + 1 < buflen)
      buf[j++] = '%';
   buf[j] = '\0';
}

int db2_code_index_term_find(const char *identifier, term_hit_t *out, int max)
{
   if (!identifier || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Hidden path rows are never valid index evidence. Filter both project
    * roots and project-relative file paths so stale rows created before the
    * scanner guard landed cannot surface through read APIs. */
   static const char *sql = "SELECT p.name, f.path, t.line, t.kind, t.line_end"
                            " FROM terms t"
                            " JOIN files f ON f.id = t.file_id"
                            " JOIN projects p ON p.id = f.project_id"
                            " WHERE t.name = ?1"
                            "   AND f.path NOT LIKE '.%'"
                            "   AND f.path NOT LIKE '%/.%'"
                            "   AND p.root NOT LIKE '%/.%'"
                            " GROUP BY p.name, f.path, t.line, t.kind, t.line_end"
                            " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                            " p.name, f.path"
                            " LIMIT ?2";
   static const char *sql_like = "SELECT p.name, f.path, t.line, t.kind, t.line_end"
                                 " FROM terms t"
                                 " JOIN files f ON f.id = t.file_id"
                                 " JOIN projects p ON p.id = f.project_id"
                                 " WHERE t.name LIKE ?1 ESCAPE '\\'"
                                 "   AND f.path NOT LIKE '.%'"
                                 "   AND f.path NOT LIKE '%/.%'"
                                 "   AND p.root NOT LIKE '%/.%'"
                                 " GROUP BY p.name, f.path, t.line, t.kind, t.line_end"
                                 " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                                 " p.name, f.path"
                                 " LIMIT ?2";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identifier);
   aimee_pg_bind_int(st, "?2", max);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *p = aimee_pg_column_text(st, 0);
      const char *f = aimee_pg_column_text(st, 1);
      int line = aimee_pg_column_int(st, 2);
      const char *k = aimee_pg_column_text(st, 3);
      snprintf(out[count].project, sizeof(out[count].project), "%s", p ? p : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f ? f : "");
      out[count].line = line;
      out[count].line_end = aimee_pg_column_int(st, 4);
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
      count++;
   }
   aimee_pg_finalize(st);

   /* Two LIKE fallbacks if exact match was empty: prefix first (preserves
    * the precision of "find aimee_db_t" → "aimee_db_t*"), then substring
    * (so "find qdrant" still finds kb_test_qdrant_post_handler). Each tier
    * only fires when the previous one returned zero rows. */
   for (int tier = 0; count == 0 && tier < 2; tier++)
   {
      char pattern[512];
      cidx_make_like_pattern(identifier, tier == 1, 1, pattern, sizeof(pattern));
      aimee_pg_stmt_t *st2 = aimee_pg_prepare(conn, sql_like, err, sizeof(err));
      if (!st2)
         break;
      aimee_pg_bind_text(st2, "?1", pattern);
      aimee_pg_bind_int(st2, "?2", max);
      while (count < max && aimee_pg_step(st2, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *p = aimee_pg_column_text(st2, 0);
         const char *f = aimee_pg_column_text(st2, 1);
         int line = aimee_pg_column_int(st2, 2);
         const char *k = aimee_pg_column_text(st2, 3);
         snprintf(out[count].project, sizeof(out[count].project), "%s", p ? p : "");
         snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f ? f : "");
         out[count].line = line;
         out[count].line_end = aimee_pg_column_int(st2, 4);
         snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
         count++;
      }
      aimee_pg_finalize(st2);
   }
   return count;
}

int db2_code_index_callers_find(const char *project, const char *symbol, caller_hit_t *out, int max)
{
   if (!symbol || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   const int filter_project = (project && project[0]) ? 1 : 0;
   static const char *sql_filtered = "SELECT p.name, f.path, cc.caller, cc.line"
                                     " FROM code_calls cc"
                                     " JOIN files f ON f.id = cc.file_id"
                                     " JOIN projects p ON p.id = f.project_id"
                                     " WHERE cc.callee = ?1 AND p.name = ?2"
                                     "   AND f.path NOT LIKE '.%'"
                                     "   AND f.path NOT LIKE '%/.%'"
                                     "   AND p.root NOT LIKE '%/.%'"
                                     " ORDER BY p.name, f.path, cc.line"
                                     " LIMIT ?3";
   static const char *sql_all = "SELECT p.name, f.path, cc.caller, cc.line"
                                " FROM code_calls cc"
                                " JOIN files f ON f.id = cc.file_id"
                                " JOIN projects p ON p.id = f.project_id"
                                " WHERE cc.callee = ?1"
                                "   AND f.path NOT LIKE '.%'"
                                "   AND f.path NOT LIKE '%/.%'"
                                "   AND p.root NOT LIKE '%/.%'"
                                " ORDER BY p.name, f.path, cc.line"
                                " LIMIT ?2";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, filter_project ? sql_filtered : sql_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", symbol);
   if (filter_project)
   {
      aimee_pg_bind_text(st, "?2", project);
      aimee_pg_bind_int(st, "?3", max);
   }
   else
   {
      aimee_pg_bind_int(st, "?2", max);
   }

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *pname = aimee_pg_column_text(st, 0);
      const char *fpath = aimee_pg_column_text(st, 1);
      const char *caller = aimee_pg_column_text(st, 2);
      int line = aimee_pg_column_int(st, 3);
      snprintf(out[count].project, sizeof(out[count].project), "%s", pname ? pname : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", fpath ? fpath : "");
      snprintf(out[count].caller, sizeof(out[count].caller), "%s", caller ? caller : "");
      out[count].line = line;
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_code_index_file_definitions(const char *project, const char *file_path, definition_t *out,
                                    int max)
{
   if (!project || !file_path || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   int64_t project_id = code_index_resolve_project(conn, project);
   if (project_id < 0)
      return 0;
   int64_t file_id = code_index_resolve_file(conn, project_id, file_path);
   if (file_id < 0)
      return 0;

   static const char *sql = "SELECT name, kind, line, line_end FROM terms"
                            " WHERE file_id = ?1 AND kind = 'definition'"
                            " ORDER BY line";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", file_id);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *n = aimee_pg_column_text(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      int line = aimee_pg_column_int(st, 2);
      snprintf(out[count].name, sizeof(out[count].name), "%s", n ? n : "");
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
      out[count].line = line;
      out[count].line_end = aimee_pg_column_int(st, 3);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_code_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   if (!project || !file_path || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   int64_t project_id = code_index_resolve_project(conn, project);
   if (project_id < 0)
      return -1;
   int64_t file_id = code_index_resolve_file(conn, project_id, file_path);

   /* Stage 1: file's exports (only used to gate dependent search). */
   int has_exports = 0;
   if (file_id >= 0)
   {
      static const char *exp_sql = "SELECT name FROM file_exports WHERE file_id = ?1 LIMIT 1";
      char err[CIDX_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, exp_sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
            has_exports = 1;
         aimee_pg_finalize(st);
      }
   }

   /* Stage 2: dependents — other files in this project that import
    * something matching the file_path. */
   if (has_exports)
   {
      static const char *dep_sql = "SELECT DISTINCT f.path FROM file_imports fi"
                                   " JOIN files f ON f.id = fi.file_id"
                                   " WHERE f.project_id = ?1 AND fi.name LIKE ?2";
      char err[CIDX_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, dep_sql, err, sizeof(err));
      if (st)
      {
         char pattern[MAX_PATH_LEN];
         snprintf(pattern, sizeof(pattern), "%%%s%%", file_path);
         aimee_pg_bind_int64(st, "?1", project_id);
         aimee_pg_bind_text(st, "?2", pattern);
         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && out->dependent_count < 64)
         {
            const char *p = aimee_pg_column_text(st, 0);
            if (p && strcmp(p, file_path) != 0)
            {
               snprintf(out->dependents[out->dependent_count], MAX_PATH_LEN, "%s", p);
               out->dependent_count++;
            }
         }
         aimee_pg_finalize(st);
      }
   }

   /* Stage 3: dependencies — what this file imports. */
   if (file_id >= 0)
   {
      static const char *imp_sql = "SELECT DISTINCT name FROM file_imports WHERE file_id = ?1";
      char err[CIDX_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, imp_sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && out->dependency_count < 64)
         {
            const char *t = aimee_pg_column_text(st, 0);
            if (t)
            {
               snprintf(out->dependencies[out->dependency_count], MAX_PATH_LEN, "%s", t);
               out->dependency_count++;
            }
         }
         aimee_pg_finalize(st);
      }
   }

   return 0;
}

int64_t db2_code_index_project_upsert(const char *name, const char *root)
{
   if (!name || !root)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO projects (name, root, scanned_at)"
                            " VALUES (?1, ?2, ?3)"
                            " ON CONFLICT(name) DO UPDATE SET root = ?2, scanned_at = ?3"
                            " RETURNING id";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_text(st, "?2", root);
   aimee_pg_bind_text(st, "?3", ts);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int64_t db2_code_index_file_upsert(int64_t project_id, const char *rel_path, const char *scanned_at)
{
   if (!rel_path || !scanned_at)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "INSERT INTO files (project_id, path, scanned_at)"
                            " VALUES (?1, ?2, ?3)"
                            " ON CONFLICT(project_id, path) DO UPDATE SET scanned_at = ?3"
                            " RETURNING id";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", rel_path);
   aimee_pg_bind_text(st, "?3", scanned_at);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_code_index_file_modified_since(int64_t project_id, const char *rel_path, time_t mtime)
{
   if (!rel_path)
      return 1;
   void *conn = db2_conn();
   if (!conn)
      return 1;

   static const char *sql = "SELECT scanned_at FROM files"
                            " WHERE project_id = ?1 AND path = ?2";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", rel_path);

   int modified = 1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ts = aimee_pg_column_text(st, 0);
      if (ts)
      {
         struct tm tmv;
         memset(&tmv, 0, sizeof(tmv));
         if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour,
                    &tmv.tm_min, &tmv.tm_sec) == 6)
         {
            tmv.tm_year -= 1900;
            tmv.tm_mon -= 1;
            time_t scanned = timegm(&tmv);
            if (scanned >= mtime)
               modified = 0;
         }
      }
   }
   aimee_pg_finalize(st);
   return modified;
}

int db2_code_index_purge_files_matching(int64_t project_id, const char *path_glob)
{
   if (!path_glob)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "DELETE FROM files WHERE project_id = ?1 AND path LIKE ?2";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", path_glob);
   int affected = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      affected = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return affected;
}

int db2_code_index_purge_hidden_pollution(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   int total = 0;
   char err[CIDX_ERRBUF] = "";

   /* Cross-project file purge: every file whose project-relative path has
    * any hidden component. CASCADE drops dependent rows. */
   static const char *files_sql = "DELETE FROM files WHERE path LIKE '.%' OR path LIKE '%/.%'";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, files_sql, err, sizeof(err));
   if (!st)
      return -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      total += aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);

   /* Drop project rows whose root is itself inside a hidden directory.
    * Matches `/foo/.aimee/...`, `/foo/.worktrees/...`, etc. */
   static const char *projects_sql = "DELETE FROM projects WHERE root LIKE '%/.%'";
   st = aimee_pg_prepare(conn, projects_sql, err, sizeof(err));
   if (!st)
      return total; /* files purge succeeded; report what we have */
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      total += aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);

   return total;
}

int db2_code_index_file_replace(int64_t file_id, const code_index_file_data_t *data)
{
   if (!data)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CIDX_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   int rc = 0;
   if (db2_exec_conn_int64(conn, "DELETE FROM file_exports WHERE file_id = ?1", file_id) != 0)
      rc = -1;
   if (db2_exec_conn_int64(conn, "DELETE FROM file_imports WHERE file_id = ?1", file_id) != 0)
      rc = -1;
   if (db2_exec_conn_int64(conn, "DELETE FROM terms WHERE file_id = ?1", file_id) != 0)
      rc = -1;
   if (db2_exec_conn_int64(conn, "DELETE FROM code_calls WHERE file_id = ?1", file_id) != 0)
      rc = -1;

   if (rc == 0)
   {
      static const char *upsert = "INSERT INTO file_contents (file_id, content)"
                                  " VALUES (?1, ?2)"
                                  " ON CONFLICT(file_id) DO UPDATE SET content = ?2";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, upsert, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         aimee_pg_bind_text(st, "?2", data->content ? data->content : "");
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = -1;
         aimee_pg_finalize(st);
      }
      else
         rc = -1;
   }

   if (rc == 0 && data->export_count > 0 && data->exports)
   {
      static const char *ins = "INSERT INTO file_exports (file_id, name) VALUES (?1, ?2)";
      for (int i = 0; i < data->export_count; i++)
      {
         aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
         if (!st)
         {
            rc = -1;
            break;
         }
         aimee_pg_bind_int64(st, "?1", file_id);
         aimee_pg_bind_text(st, "?2", data->exports[i] ? data->exports[i] : "");
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = -1;
         aimee_pg_finalize(st);
         if (rc != 0)
            break;
      }
   }

   if (rc == 0 && data->import_count > 0 && data->imports)
   {
      static const char *ins = "INSERT INTO file_imports (file_id, name) VALUES (?1, ?2)";
      for (int i = 0; i < data->import_count; i++)
      {
         aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
         if (!st)
         {
            rc = -1;
            break;
         }
         aimee_pg_bind_int64(st, "?1", file_id);
         aimee_pg_bind_text(st, "?2", data->imports[i] ? data->imports[i] : "");
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = -1;
         aimee_pg_finalize(st);
         if (rc != 0)
            break;
      }
   }

   if (rc == 0 && data->route_count > 0 && data->routes)
   {
      static const char *ins = "INSERT INTO terms (file_id, name, kind, line)"
                               " VALUES (?1, ?2, 'route', 0)";
      for (int i = 0; i < data->route_count; i++)
      {
         aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
         if (!st)
         {
            rc = -1;
            break;
         }
         aimee_pg_bind_int64(st, "?1", file_id);
         aimee_pg_bind_text(st, "?2", data->routes[i] ? data->routes[i] : "");
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = -1;
         aimee_pg_finalize(st);
         if (rc != 0)
            break;
      }
   }

   if (rc == 0 && data->definition_count > 0 && data->definitions)
   {
      static const char *ins = "INSERT INTO terms (file_id, name, kind, line, line_end)"
                               " VALUES (?1, ?2, ?3, ?4, ?5)";
      for (int i = 0; i < data->definition_count; i++)
      {
         aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
         if (!st)
         {
            rc = -1;
            break;
         }
         aimee_pg_bind_int64(st, "?1", file_id);
         aimee_pg_bind_text(st, "?2", data->definitions[i].name);
         aimee_pg_bind_text(st, "?3", data->definitions[i].kind);
         aimee_pg_bind_int(st, "?4", data->definitions[i].line);
         aimee_pg_bind_int(st, "?5", data->definitions[i].line_end);
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = -1;
         aimee_pg_finalize(st);
         if (rc != 0)
            break;
      }
   }

   if (rc == 0 && data->call_count > 0 && data->calls)
   {
      static const char *ins = "INSERT INTO code_calls (file_id, caller, callee, line)"
                               " VALUES (?1, ?2, ?3, ?4)";
      for (int i = 0; i < data->call_count; i++)
      {
         aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
         if (!st)
         {
            rc = -1;
            break;
         }
         aimee_pg_bind_int64(st, "?1", file_id);
         aimee_pg_bind_text(st, "?2", data->calls[i].caller);
         aimee_pg_bind_text(st, "?3", data->calls[i].callee);
         aimee_pg_bind_int(st, "?4", data->calls[i].line);
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = -1;
         aimee_pg_finalize(st);
         if (rc != 0)
            break;
      }
   }

   if (rc == 0)
      aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
   else
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc;
}

int db2_code_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                               int max, int enrich)
{
   if (!query || !query[0] || !out || max <= 0)
      return query && !query[0] ? 0 : -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   const int filter_project = (project && project[0]) ? 1 : 0;
   const int shim = aimee_pg_is_shim();

   /* P1b span enrichment: when `enrich`, also fetch the matched file's content as
    * a 6th column so the matched line can be located server-side (the int line
    * crosses the wire, not the content). The pg path already JOINs file_contents
    * (fc.content); the shim path JOINs it in only when enriching. When !enrich the
    * composed SQL is the same query (same plan, result set, and cost) as the
    * prior literals — the default-off guarantee. */
   const char *sel_content = !enrich ? "" : (shim ? ", fcs.content" : ", fc.content");
   const char *join_content =
       (enrich && shim) ? " JOIN file_contents fcs ON fcs.file_id = f.id" : "";

   /* The DB2 shim path exposes snippet/rank, while the primary DB2 path uses
    * ts_headline/ts_rank. The same JOIN shape works because `code_fts`
    * presents rowid + fts_tsv columns over file_contents. */
   char sql[1400];
   if (shim && filter_project)
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " snippet(code_fts, 0, '>>>', '<<<', '...', 20), rank, f.hash%s"
               " FROM code_fts JOIN files f ON f.id = code_fts.rowid"
               " JOIN projects p ON p.id = f.project_id%s"
               " WHERE code_fts MATCH ?1 AND p.name = ?2"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%' ORDER BY rank LIMIT ?3",
               sel_content, join_content);
   else if (shim)
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " snippet(code_fts, 0, '>>>', '<<<', '...', 20), rank, f.hash%s"
               " FROM code_fts JOIN files f ON f.id = code_fts.rowid"
               " JOIN projects p ON p.id = f.project_id%s"
               " WHERE code_fts MATCH ?1"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%' ORDER BY rank LIMIT ?2",
               sel_content, join_content);
   else if (filter_project)
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " ts_headline('simple', fc.content, plainto_tsquery('simple', ?1),"
               " 'StartSel=>>>, StopSel=<<<, MaxWords=20'),"
               " ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)), f.hash%s"
               " FROM file_contents fc JOIN files f ON f.id = fc.file_id"
               " JOIN projects p ON p.id = f.project_id"
               " WHERE fc.code_fts_tsv @@ plainto_tsquery('simple', ?1) AND p.name = ?2"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%'"
               " ORDER BY ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)) DESC LIMIT ?3",
               sel_content);
   else
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " ts_headline('simple', fc.content, plainto_tsquery('simple', ?1),"
               " 'StartSel=>>>, StopSel=<<<, MaxWords=20'),"
               " ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)), f.hash%s"
               " FROM file_contents fc JOIN files f ON f.id = fc.file_id"
               " JOIN projects p ON p.id = f.project_id"
               " WHERE fc.code_fts_tsv @@ plainto_tsquery('simple', ?1)"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%'"
               " ORDER BY ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)) DESC LIMIT ?2",
               sel_content);

   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", query);
   if (filter_project)
   {
      aimee_pg_bind_text(st, "?2", project);
      aimee_pg_bind_int(st, "?3", max);
   }
   else
   {
      aimee_pg_bind_int(st, "?2", max);
   }

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *pname = aimee_pg_column_text(st, 0);
      const char *fpath = aimee_pg_column_text(st, 1);
      const char *snip = aimee_pg_column_text(st, 2);
      double rnk = aimee_pg_column_double(st, 3);
      const char *fhash = aimee_pg_column_text(st, 4); /* files.hash — P2 provenance */
      snprintf(out[count].project, sizeof(out[count].project), "%s", pname ? pname : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", fpath ? fpath : "");
      snprintf(out[count].snippet, sizeof(out[count].snippet), "%s", snip ? snip : "");
      out[count].rank = rnk;
      snprintf(out[count].content_hash, sizeof(out[count].content_hash), "%s", fhash ? fhash : "");
      /* P1b: locate the matched line from the file content (column 5), without
       * copying the content out. 0 when not enriching or not locatable. */
      out[count].line = 0;
      if (enrich)
      {
         const char *content = aimee_pg_column_text(st, 5);
         if (content && snip)
            out[count].line = code_match_line(content, snip);
      }
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}
