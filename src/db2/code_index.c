/* db2/code_index.c: code-index primitives — Postgres via libpq. */

#include "code_index.h"
#include "../headers/aimee.h"      /* MAX_PATH_LEN, now_utc */
#include "../headers/code_match.h" /* code_match_line (P1b span enrichment) */
#include "cross_repo_resolver.h"   /* H0b: xrepo_lang_name / xrepo_path_is_vendored */
#include "../headers/log.h"        /* aimee_log */
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "util.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIDX_ERRBUF 256

static int64_t code_index_resolve_project(void *conn, const char *name)
{
   if (!conn || !name)
      return -1;
   static const char *sql =
       "SELECT id FROM projects WHERE name = ?1 AND lifecycle_state = 'current'";
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
   static const char *sql = "SELECT f.id FROM files f"
                            " JOIN projects p ON p.id=f.project_id"
                            " WHERE f.project_id = ?1 AND f.path = ?2"
                            " AND p.lifecycle_state='current'"
                            " AND f.generation=p.current_generation";
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
   static const char *sql = "SELECT COUNT(*) FROM projects WHERE lifecycle_state = 'current'";
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

int db2_code_index_project_current_generation(const char *name, int64_t *generation_out)
{
   if (generation_out)
      *generation_out = 0;
   if (!name || !name[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT current_generation FROM projects WHERE name = ?1 AND lifecycle_state = 'current'",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   int rc = -2;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (generation_out)
         *generation_out = aimee_pg_column_int64(st, 0);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_code_index_project_last_scan(char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "SELECT MAX(scanned_at) FROM projects WHERE lifecycle_state = 'current'";
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
                            " WHERE lifecycle_state = 'current' AND root NOT LIKE '%/.%'"
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
                            "   AND p.lifecycle_state = 'current'"
                            "   AND f.generation = p.current_generation"
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
                                 "   AND p.lifecycle_state = 'current'"
                                 "   AND f.generation = p.current_generation"
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
                                     "   AND p.lifecycle_state = 'current'"
                                     "   AND f.generation = p.current_generation"
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
                                "   AND p.lifecycle_state = 'current'"
                                "   AND f.generation = p.current_generation"
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
   {
      /* Four different refusals used to return a bare -1, which the route turned
       * into 404 and the client into "blast radius lookup failed". An operator
       * could not tell an unknown project from an unindexed file from a
       * generation the row does not carry -- and the first two are actionable. */
      aimee_log(LOG_ERROR, "code_index", "blast_radius '%s' '%s': no db2 connection", project,
                file_path);
      return -1;
   }

   int64_t project_id = code_index_resolve_project(conn, project);
   if (project_id < 0)
   {
      aimee_log(LOG_ERROR, "code_index", "blast_radius: unknown project '%s'", project);
      return -1;
   }
   int64_t file_id = code_index_resolve_file(conn, project_id, file_path);
   if (file_id < 0)
   {
      aimee_log(LOG_ERROR, "code_index",
                "blast_radius: project '%s' has no indexed file '%s' at its current generation",
                project, file_path);
      return -1;
   }

   int64_t generation = 0;
   if (db2_code_index_project_current_generation(project, &generation) != 0)
   {
      aimee_log(LOG_ERROR, "code_index", "blast_radius: project '%s' has no current generation",
                project);
      return -1;
   }
   snprintf(out->project, sizeof(out->project), "%s", project);
   out->generation = (long long)generation;
   snprintf(out->freshness, sizeof(out->freshness), "current");
   out->resolved = 1;

   char err[CIDX_ERRBUF] = "";

   /* Local imports are examined in C because relative Python identities need
    * the importing path. Exact normalized equality is the authority; SQL LIKE
    * and unescaped substring matching are deliberately absent. */
   {
      static const char *sql = "SELECT f.path, fi.name FROM file_imports fi"
                               " JOIN files f ON f.id=fi.file_id"
                               " JOIN projects p ON p.id=f.project_id"
                               " WHERE f.project_id=?1 AND p.lifecycle_state='current'"
                               " AND f.generation=p.current_generation ORDER BY f.path,fi.name";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_int64(st, "?1", project_id);
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *importer = aimee_pg_column_text(st, 0);
         const char *raw = aimee_pg_column_text(st, 1);
         if (!importer || !raw || strcmp(importer, file_path) == 0 ||
             !code_import_resolves_path(importer, raw, file_path))
            continue;
         int found = -1;
         for (int i = 0; i < out->dependent_count; i++)
            if (strcmp(out->dependents[i], importer) == 0 &&
                strcmp(out->dependent_meta[i].project, project) == 0)
               found = i;
         if (found < 0 && out->dependent_count < 64)
         {
            found = out->dependent_count++;
            snprintf(out->dependents[found], MAX_PATH_LEN, "%s", importer);
            snprintf(out->dependent_meta[found].project, sizeof(out->dependent_meta[found].project),
                     "%s", project);
            out->dependent_meta[found].generation = (long long)generation;
            snprintf(out->dependent_meta[found].freshness,
                     sizeof(out->dependent_meta[found].freshness), "current");
         }
         if (found >= 0)
         {
            snprintf(out->dependent_meta[found].provenance,
                     sizeof(out->dependent_meta[found].provenance), "import");
            snprintf(out->dependent_meta[found].confidence,
                     sizeof(out->dependent_meta[found].confidence), "high");
         }
      }
      aimee_pg_finalize(st);
   }

   /* Direct calls are authoritative only for exports unique in the current
    * project; ambiguous same-name exports cannot be resolved by code_calls. */
   {
      static const char *sql =
          /* The uniqueness test is a property of the PROJECT, not of the row being
           * examined, but it used to be a correlated subquery: a three-table join
           * re-executed once per candidate call row. Measured on a 3825-file
           * checkout that was 7796 ms of a 6.4 s lookup -- 99% of blast-radius --
           * and it scaled with project size, so it was invisible on small fixtures
           * and crippling on real ones. Hoisting it into a CTE computes the set
           * once: 56 ms for identical results (verified by EXCEPT in both
           * directions on the same project). */
          "WITH unique_exports AS ("
          " SELECT other.name FROM file_exports other"
          " JOIN files ofile ON ofile.id=other.file_id"
          " JOIN projects op ON op.id=ofile.project_id"
          " WHERE ofile.project_id=?2 AND op.lifecycle_state='current'"
          " AND ofile.generation=op.current_generation"
          " GROUP BY other.name HAVING COUNT(*)=1)"
          " SELECT DISTINCT f.path FROM code_calls cc"
          " JOIN files f ON f.id=cc.file_id JOIN projects p ON p.id=f.project_id"
          " JOIN file_exports target ON target.file_id=?1 AND target.name=cc.callee"
          " JOIN unique_exports ue ON ue.name=cc.callee"
          " WHERE f.project_id=?2 AND f.id<>?1 AND p.lifecycle_state='current'"
          " AND f.generation=p.current_generation"
          " ORDER BY f.path";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_int64(st, "?1", file_id);
      aimee_pg_bind_int64(st, "?2", project_id);
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *caller = aimee_pg_column_text(st, 0);
         if (!caller)
            continue;
         int found = -1;
         for (int i = 0; i < out->dependent_count; i++)
            if (strcmp(out->dependents[i], caller) == 0 &&
                strcmp(out->dependent_meta[i].project, project) == 0)
               found = i;
         if (found < 0 && out->dependent_count < 64)
         {
            found = out->dependent_count++;
            snprintf(out->dependents[found], MAX_PATH_LEN, "%s", caller);
            snprintf(out->dependent_meta[found].project, sizeof(out->dependent_meta[found].project),
                     "%s", project);
            out->dependent_meta[found].generation = (long long)generation;
            snprintf(out->dependent_meta[found].freshness,
                     sizeof(out->dependent_meta[found].freshness), "current");
         }
         if (found >= 0)
         {
            char *provenance = out->dependent_meta[found].provenance;
            if (provenance[0] && !strstr(provenance, "call"))
               strncat(provenance, ",call",
                       sizeof(out->dependent_meta[found].provenance) - strlen(provenance) - 1);
            else if (!provenance[0])
               snprintf(provenance, sizeof(out->dependent_meta[found].provenance), "call");
            snprintf(out->dependent_meta[found].confidence,
                     sizeof(out->dependent_meta[found].confidence), "high");
         }
      }
      aimee_pg_finalize(st);
   }

   /* Dependencies preserve normalized identities owned by the target file. */
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT DISTINCT name FROM file_imports WHERE file_id=?1 ORDER BY name", err,
          sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_int64(st, "?1", file_id);
      while (out->dependency_count < 64 && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *raw = aimee_pg_column_text(st, 0);
         char identity[MAX_PATH_LEN];
         if (!raw || code_import_identity(file_path, raw, identity, sizeof(identity)) != 0)
            continue;
         int found = -1;
         for (int i = 0; i < out->dependency_count; i++)
            if (strcmp(out->dependencies[i], identity) == 0)
               found = i;
         if (found >= 0)
            continue;
         found = out->dependency_count++;
         snprintf(out->dependencies[found], MAX_PATH_LEN, "%s", identity);
         snprintf(out->dependency_meta[found].provenance,
                  sizeof(out->dependency_meta[found].provenance), "import");
         snprintf(out->dependency_meta[found].confidence,
                  sizeof(out->dependency_meta[found].confidence), "high");
         snprintf(out->dependency_meta[found].project, sizeof(out->dependency_meta[found].project),
                  "%s", project);
         out->dependency_meta[found].generation = (long long)generation;
         snprintf(out->dependency_meta[found].freshness,
                  sizeof(out->dependency_meta[found].freshness), "current");
      }
      aimee_pg_finalize(st);
   }

   /* Cross-project tails are admitted only through a previously resolved
    * structural route, after all local edges, and still require exact import
    * identity resolution to this target. */
   {
      static const char *sql =
          "SELECT DISTINCT cp.name,cp.current_generation,f.path,fi.name,cr.confidence"
          " FROM cross_repo_route cr JOIN projects cp ON cp.name=cr.caller_project"
          " JOIN files f ON f.project_id=cp.id JOIN file_imports fi ON fi.file_id=f.id"
          " WHERE cr.definer_project=?1 AND cp.lifecycle_state='current'"
          " AND f.generation=cp.current_generation ORDER BY cp.name,f.path";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", project);
      while (out->dependent_count < 64 && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *caller_project = aimee_pg_column_text(st, 0);
         long long caller_generation = (long long)aimee_pg_column_int64(st, 1);
         const char *caller_path = aimee_pg_column_text(st, 2);
         const char *raw = aimee_pg_column_text(st, 3);
         const char *confidence = aimee_pg_column_text(st, 4);
         if (!caller_project || !caller_path || !raw ||
             !code_import_resolves_path(caller_path, raw, file_path))
            continue;
         int found = -1;
         for (int i = 0; i < out->dependent_count; i++)
            if (strcmp(out->dependents[i], caller_path) == 0 &&
                strcmp(out->dependent_meta[i].project, caller_project) == 0)
               found = i;
         if (found < 0)
         {
            found = out->dependent_count++;
            snprintf(out->dependents[found], MAX_PATH_LEN, "%s", caller_path);
            snprintf(out->dependent_meta[found].project, sizeof(out->dependent_meta[found].project),
                     "%s", caller_project);
            out->dependent_meta[found].generation = caller_generation;
            snprintf(out->dependent_meta[found].freshness,
                     sizeof(out->dependent_meta[found].freshness), "current");
         }
         snprintf(out->dependent_meta[found].provenance,
                  sizeof(out->dependent_meta[found].provenance), "cross_repo");
         snprintf(out->dependent_meta[found].confidence,
                  sizeof(out->dependent_meta[found].confidence), "%s",
                  confidence && confidence[0] ? confidence : "medium");
      }
      aimee_pg_finalize(st);
   }

   return 0;
}

void db2_code_index_blast_radius_local_first(const char *project, blast_radius_t *out)
{
   if (!project || !project[0] || !out || out->dependent_count < 2)
      return;

   int first_external = -1;
   for (int i = 0; i < out->dependent_count; i++)
   {
      const int local = strcmp(out->dependent_meta[i].project, project) == 0;
      if (!local)
      {
         if (first_external < 0)
            first_external = i;
         continue;
      }
      if (first_external < 0)
         continue;

      char path[MAX_PATH_LEN];
      blast_edge_meta_t meta = out->dependent_meta[i];
      memcpy(path, out->dependents[i], sizeof(path));
      memmove(&out->dependents[first_external + 1], &out->dependents[first_external],
              (size_t)(i - first_external) * sizeof(out->dependents[0]));
      memmove(&out->dependent_meta[first_external + 1], &out->dependent_meta[first_external],
              (size_t)(i - first_external) * sizeof(out->dependent_meta[0]));
      memcpy(out->dependents[first_external], path, sizeof(path));
      out->dependent_meta[first_external] = meta;
      first_external++;
   }
}

int db2_code_index_unique_file_basename(const char *project, const char *basename, char *out,
                                        size_t out_cap)
{
   if (!project || !basename || !out || out_cap == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int64_t project_id = code_index_resolve_project(conn, project);
   if (project_id < 0)
      return -1;
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT f.path FROM files f JOIN projects p ON p.id=f.project_id"
                        " WHERE f.project_id=?1 AND p.lifecycle_state='current'"
                        " AND f.generation=p.current_generation ORDER BY f.path",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
   int matches = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *path = aimee_pg_column_text(st, 0);
      const char *base = path ? strrchr(path, '/') : NULL;
      base = base ? base + 1 : path;
      if (base && strcmp(base, basename) == 0)
      {
         matches++;
         if (matches == 1)
            snprintf(out, out_cap, "%s", path);
      }
   }
   aimee_pg_finalize(st);
   if (matches != 1)
      out[0] = '\0';
   return matches == 1 ? 1 : 0;
}

int64_t db2_code_index_project_upsert(const char *name, const char *root)
{
   if (!name || !name[0] || !root || !root[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[CIDX_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   int64_t id = -1;
   int64_t generation = 1;
   char old_root[MAX_PATH_LEN] = "";
   char old_state[24] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       aimee_pg_is_shim()
           ? "SELECT id, root, lifecycle_state, current_generation FROM projects WHERE name = ?1"
           : "SELECT id, root, lifecycle_state, current_generation FROM projects WHERE name = ?1 "
             "FOR UPDATE",
       err, sizeof(err));
   if (!st)
      goto rollback;
   aimee_pg_bind_text(st, "?1", name);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      id = aimee_pg_column_int64(st, 0);
      const char *stored_root = aimee_pg_column_text(st, 1);
      const char *stored_state = aimee_pg_column_text(st, 2);
      snprintf(old_root, sizeof(old_root), "%s", stored_root ? stored_root : "");
      snprintf(old_state, sizeof(old_state), "%s", stored_state ? stored_state : "current");
      generation = aimee_pg_column_int64(st, 3);
      if (generation < 1)
         generation = 1;
   }
   aimee_pg_finalize(st);

   if (id < 0)
   {
      st = aimee_pg_prepare(
          conn,
          "INSERT INTO projects (name, root, scanned_at, lifecycle_state, current_generation)"
          " VALUES (?1, ?2, ?3, 'current', 1) RETURNING id",
          err, sizeof(err));
      if (!st)
         goto rollback;
      aimee_pg_bind_text(st, "?1", name);
      aimee_pg_bind_text(st, "?2", root);
      aimee_pg_bind_text(st, "?3", ts);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         id = aimee_pg_column_int64(st, 0);
      aimee_pg_finalize(st);
      if (id < 0)
         goto rollback;
   }
   else
   {
      int root_changed = strcmp(old_root, root) != 0;
      int reattach = strcmp(old_state, "current") != 0;
      if (reattach)
      {
         st = aimee_pg_prepare(conn,
                               "UPDATE code_project_generations SET state = 'superseded',"
                               " detached_at = ?1 WHERE project_id = ?2 AND state = 'current'",
                               err, sizeof(err));
         if (!st)
            goto rollback;
         aimee_pg_bind_text(st, "?1", ts);
         aimee_pg_bind_int64(st, "?2", id);
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
         {
            aimee_pg_finalize(st);
            goto rollback;
         }
         aimee_pg_finalize(st);

         generation++;
      }

      /* A checkout move changes the current alias and generation root, but it
       * is not a new indexing generation. Only reattaching a project that was
       * explicitly detached advances the generation. */
      if (root_changed || reattach)
      {
         st = aimee_pg_prepare(conn,
                               "UPDATE code_project_aliases SET is_current = 0"
                               " WHERE project_id = ?1 AND is_current = 1",
                               err, sizeof(err));
         if (!st)
            goto rollback;
         aimee_pg_bind_int64(st, "?1", id);
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
         {
            aimee_pg_finalize(st);
            goto rollback;
         }
         aimee_pg_finalize(st);
      }

      st = aimee_pg_prepare(conn,
                            "UPDATE projects SET root = ?1, scanned_at = ?2,"
                            " lifecycle_state = 'current', current_generation = ?3 WHERE id = ?4",
                            err, sizeof(err));
      if (!st)
         goto rollback;
      aimee_pg_bind_text(st, "?1", root);
      aimee_pg_bind_text(st, "?2", ts);
      aimee_pg_bind_int64(st, "?3", generation);
      aimee_pg_bind_int64(st, "?4", id);
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(st);
         goto rollback;
      }
      aimee_pg_finalize(st);
   }

   st = aimee_pg_prepare(
       conn,
       "INSERT INTO code_project_generations"
       " (project_id, generation, root, state, created_at, detached_at)"
       " VALUES (?1, ?2, ?3, 'current', ?4, '')"
       " ON CONFLICT(project_id, generation) DO UPDATE SET root = ?3, state = 'current',"
       " detached_at = ''",
       err, sizeof(err));
   if (!st)
      goto rollback;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_bind_int64(st, "?2", generation);
   aimee_pg_bind_text(st, "?3", root);
   aimee_pg_bind_text(st, "?4", ts);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(st);
      goto rollback;
   }
   aimee_pg_finalize(st);

   /* Only concrete checkout paths are aliases. Labels such as "remote" are
    * intentionally not globally unique and must never collide. */
   if (root[0] == '/')
   {
      st = aimee_pg_prepare(conn, "SELECT project_id FROM code_project_aliases WHERE alias = ?1",
                            err, sizeof(err));
      if (!st)
         goto rollback;
      aimee_pg_bind_text(st, "?1", root);
      int64_t alias_owner = -1;
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         alias_owner = aimee_pg_column_int64(st, 0);
      aimee_pg_finalize(st);
      /* A checkout claimed by another project is a RE-INDEX under a new name,
       * not an error. Rejecting it rolled the whole upsert back and returned a
       * bare -1, which surfaced as "canonical index scan failed" with nothing
       * logged -- and it was permanent, because the alias never moved. Any
       * caller that mints a fresh project name per attempt (a retry that must
       * not read a previous attempt's rows, for instance) could scan a
       * directory exactly once, ever.
       *
       * The alias is "who owns this checkout NOW", and the model already has
       * is_current for that: a checkout MOVING to a new path is handled a few
       * lines above. This is the same event from the other side. Hand the alias
       * over, leave the old project's rows alone, and say so -- a silent
       * transfer would be as bad as the silent refusal. */
      if (alias_owner >= 0 && alias_owner != id)
         aimee_log(LOG_INFO, "code_index",
                   "checkout '%s' reindexed under project '%s' (was project id %lld)", root, name,
                   (long long)alias_owner);

      st = aimee_pg_prepare(
          conn,
          "INSERT INTO code_project_aliases"
          " (project_id, alias, alias_kind, is_current, first_seen_at, last_seen_at)"
          " VALUES (?1, ?2, 'checkout', 1, ?3, ?3)"
          " ON CONFLICT(alias) DO UPDATE SET project_id = ?1, is_current = 1, last_seen_at = ?3",
          err, sizeof(err));
      if (!st)
         goto rollback;
      aimee_pg_bind_int64(st, "?1", id);
      aimee_pg_bind_text(st, "?2", root);
      aimee_pg_bind_text(st, "?3", ts);
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(st);
         goto rollback;
      }
      aimee_pg_finalize(st);
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      goto rollback;
   return id;

rollback:
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return -1;
}

int64_t db2_code_index_file_upsert(int64_t project_id, const char *rel_path, const char *scanned_at)
{
   if (!rel_path || !scanned_at)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* H0b: per-file language + vendored flag, derived from the path at index time. */
   const char *language = xrepo_lang_name(xrepo_lang_from_path(rel_path));
   int vendored = xrepo_path_is_vendored(rel_path);
   static const char *sql =
       "INSERT INTO files (project_id, generation, path, scanned_at, language, vendored)"
       " SELECT ?1, current_generation, ?2, ?3, ?4, ?5 FROM projects"
       " WHERE id=?1 AND lifecycle_state='current'"
       " ON CONFLICT(project_id, generation, path) DO UPDATE SET scanned_at = ?3,"
       " language = ?4, vendored = ?5"
       " RETURNING id";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", rel_path);
   aimee_pg_bind_text(st, "?3", scanned_at);
   aimee_pg_bind_text(st, "?4", language);
   aimee_pg_bind_int(st, "?5", vendored);
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

   static const char *sql = "SELECT f.scanned_at FROM files f"
                            " JOIN projects p ON p.id=f.project_id"
                            " WHERE f.project_id = ?1 AND f.path = ?2"
                            " AND p.lifecycle_state='current'"
                            " AND f.generation=p.current_generation";
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
         /* Shared parser: this matched only the ISO spelling, and the column also
          * holds the canonical text form that pg_now_text() writes. An unparsed
          * stamp left `modified` at 1, so the file was re-indexed on every scan
          * -- wasteful rather than wrong, which is why nothing surfaced it. */
         time_t scanned = parse_utc_ts(ts);
         if (scanned > 0 && scanned >= mtime)
            modified = 0;
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

   static const char *sql = "DELETE FROM files WHERE project_id = ?1 AND path LIKE ?2"
                            " AND generation=(SELECT current_generation FROM projects WHERE id=?1)";
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

/* SQL predicate (on `path`) selecting a project-relative path that HAS a hidden
 * component yet is NOT a wanted dotfile build manifest, so the hidden-path purges
 * delete exactly what the ingest layer would have refused (ci_path_ingest_excluded
 * / ci_is_dotfile_manifest — recall §2.2). A .gitmodules is spared ONLY when every
 * ANCESTOR component is non-hidden: the repo-root `.gitmodules`, or `dir/.gitmodules`
 * with no hidden dir before it. A .gitmodules under a hidden ancestor (`.git/.gitmodules`,
 * `a/.hidden/.gitmodules`) is still purged — ingest never admits it. */
#define CIDX_HIDDEN_NOT_MANIFEST                                                                   \
   "(path LIKE '.%' OR path LIKE '%/.%') "                                                         \
   "AND NOT (path = '.gitmodules' OR (path LIKE '%/.gitmodules' AND path NOT LIKE '.%' "           \
   "                                  AND path NOT LIKE '%/.%/.gitmodules'))"

int db2_code_index_purge_hidden_except_manifests(int64_t project_id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Per-project hidden-path purge that spares wanted dotfile manifests. */
   static const char *sql = "DELETE FROM files WHERE project_id = ?1 AND " CIDX_HIDDEN_NOT_MANIFEST
                            " AND generation=(SELECT current_generation FROM projects WHERE id=?1)";
   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
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

   /* Cross-project file purge: every file whose project-relative path has a hidden
    * component, EXCEPT a wanted dotfile build manifest (.gitmodules with all
    * non-hidden ancestors) — git submodule declarations are legitimately indexed
    * despite the leading-'.' filename (recall §2.2); the ingest path admits them, so
    * this startup cleanup must not delete them back out. CASCADE drops dependents. */
   static const char *files_sql =
       "DELETE FROM files WHERE id IN (SELECT f.id FROM files f"
       " JOIN projects p ON p.id=f.project_id WHERE p.lifecycle_state='current'"
       " AND f.generation=p.current_generation AND " CIDX_HIDDEN_NOT_MANIFEST ")";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, files_sql, err, sizeof(err));
   if (!st)
      return -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      total += aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);

   /* A hidden checkout root is an alias/lifecycle concern, not permission to
    * erase its stable project identity. Detach/purge remain explicit audited
    * operations; startup hygiene only removes inadmissible files from the
    * current generation. */
   return total;
}

int db2_code_index_project_delete(const char *name)
{
   if (!name || !name[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CIDX_ERRBUF] = "";
   /* Children (files -> file_exports/file_imports/terms/code_calls/
    * file_contents) all cascade off the projects row (schema.sql FKs). */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM projects WHERE name = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : -1;
}

int db2_code_index_file_replace(int64_t file_id, const code_index_file_data_t *data)
{
   if (!data)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char *clean_content = strdup(data->content ? data->content : "");
   if (!clean_content)
      return -1;
   (void)text_sanitize_utf8(clean_content);

   char err[CIDX_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      free(clean_content);
      return -1;
   }

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
         aimee_pg_bind_text(st, "?2", clean_content);
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
      /* H0a: coarse kind stays 'definition' (the ~10 kind='definition' consumers,
       * incl. index_structure/index_find here, are unchanged); the extractor's
       * granular kind goes to def_kind for the cross-repo resolver (§5). */
      static const char *ins = "INSERT INTO terms (file_id, name, kind, def_kind, line, line_end)"
                               " VALUES (?1, ?2, 'definition', ?3, ?4, ?5)";
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
   {
      if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      {
         aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
         rc = -1;
      }
   }
   else
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   free(clean_content);
   return rc;
}

static int code_search_scoped(const char *query, const char *project, const char *excluded_project,
                              code_search_hit_t *out, int max, int enrich)
{
   if (!query || !query[0] || !out || max <= 0)
      return query && !query[0] ? 0 : -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   const int filter_project = (project && project[0]) ? 1 : 0;
   const int exclude_project = (excluded_project && excluded_project[0]) ? 1 : 0;
   if (filter_project && exclude_project)
      return -1;
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
   if (shim && exclude_project)
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " snippet(code_fts, 0, '>>>', '<<<', '...', 20), rank, f.hash%s"
               " FROM code_fts JOIN files f ON f.id = code_fts.rowid"
               " JOIN projects p ON p.id = f.project_id%s"
               " WHERE code_fts MATCH ?1 AND p.name <> ?2"
               "   AND p.lifecycle_state = 'current'"
               "   AND f.generation = p.current_generation"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%' ORDER BY rank LIMIT ?3",
               sel_content, join_content);
   else if (shim && filter_project)
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " snippet(code_fts, 0, '>>>', '<<<', '...', 20), rank, f.hash%s"
               " FROM code_fts JOIN files f ON f.id = code_fts.rowid"
               " JOIN projects p ON p.id = f.project_id%s"
               " WHERE code_fts MATCH ?1 AND p.name = ?2"
               "   AND p.lifecycle_state = 'current'"
               "   AND f.generation = p.current_generation"
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
               "   AND p.lifecycle_state = 'current'"
               "   AND f.generation = p.current_generation"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%' ORDER BY rank LIMIT ?2",
               sel_content, join_content);
   else if (exclude_project)
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " ts_headline('simple', fc.content, plainto_tsquery('simple', ?1),"
               " 'StartSel=>>>, StopSel=<<<, MaxWords=20'),"
               " ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)), f.hash%s"
               " FROM file_contents fc JOIN files f ON f.id = fc.file_id"
               " JOIN projects p ON p.id = f.project_id"
               " WHERE fc.code_fts_tsv @@ plainto_tsquery('simple', ?1) AND p.name <> ?2"
               "   AND p.lifecycle_state = 'current'"
               "   AND f.generation = p.current_generation"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%'"
               " ORDER BY ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)) DESC LIMIT ?3",
               sel_content);
   else if (filter_project)
      snprintf(sql, sizeof(sql),
               "SELECT p.name, f.path,"
               " ts_headline('simple', fc.content, plainto_tsquery('simple', ?1),"
               " 'StartSel=>>>, StopSel=<<<, MaxWords=20'),"
               " ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)), f.hash%s"
               " FROM file_contents fc JOIN files f ON f.id = fc.file_id"
               " JOIN projects p ON p.id = f.project_id"
               " WHERE fc.code_fts_tsv @@ plainto_tsquery('simple', ?1) AND p.name = ?2"
               "   AND p.lifecycle_state = 'current'"
               "   AND f.generation = p.current_generation"
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
               "   AND p.lifecycle_state = 'current'"
               "   AND f.generation = p.current_generation"
               "   AND f.path NOT LIKE '.%%' AND f.path NOT LIKE '%%/.%%'"
               "   AND p.root NOT LIKE '%%/.%%'"
               " ORDER BY ts_rank(fc.code_fts_tsv, plainto_tsquery('simple', ?1)) DESC LIMIT ?2",
               sel_content);

   char err[CIDX_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", query);
   if (filter_project || exclude_project)
   {
      aimee_pg_bind_text(st, "?2", filter_project ? project : excluded_project);
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

int db2_code_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                               int max, int enrich)
{
   return code_search_scoped(query, project, NULL, out, max, enrich);
}

int db2_code_index_code_search_excluding_project(const char *query, const char *excluded_project,
                                                 code_search_hit_t *out, int max, int enrich)
{
   if (!excluded_project || !excluded_project[0])
      return -1;
   return code_search_scoped(query, NULL, excluded_project, out, max, enrich);
}
