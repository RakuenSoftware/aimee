/* canonical_index_query.c: the read surface of the canonical code index.
 *
 * Project listing, symbol lookup, blast radius, structure, caller search,
 * per-project stats and code search. Split from canonical_index.c, which owns
 * the write/scan surface, to keep both under the 2500-line ceiling. The two
 * share ci_conn / ci_resolve_project_id / ci_resolve_file_id through
 * canonical_index_internal.h.
 */
#include "canonical_index.h"
#include "canonical_index_internal.h"
#include "code_index.h"
#include "cross_repo_resolver.h" /* H0b: xrepo_lang_name / xrepo_path_is_vendored */
#include "../support/db2_runtime_config.h"
#include "css_graph.h" /* CSS migration assistant: style graph + component join (WP-C/D) */
#include "db2.h"
#include "db2_bounded_text.h"
#include "db2_internal.h"
#include "entity_edges.h"     /* co_edited backfill: edge upsert / co_targets read */
#include "index.h"            /* cochange_pairs_for_commit / cochange_is_hex_sha */
#include "kb_runtime_state.h" /* db2_kb_purge_fence_active: commit-point fence check */
#include "modules/memory/memory_ontology.h" /* REL_CO_EDITED / NODE_FILE */
#include "aimee.h"
#include "db_postgres.h"
#include "../support/db2_log.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int canonical_index_list_projects(project_info_t *out, int max)
{
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT name, root, scanned_at FROM projects"
                                          " WHERE lifecycle_state = 'current'"
                                          " AND root NOT LIKE '%/.%'"
                                          " ORDER BY name",
                                          err, sizeof(err));
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

/* Build a LIKE pattern from identifier, escaping SQL wildcards.
 * C identifiers contain '_' which LIKE treats as a single-char wildcard,
 * so we escape '%', '_', and '\' before adding caller-controlled '%'
 * wildcards. leading_pct/trailing_pct select prefix vs substring matching.
 * The companion SQL must use ESCAPE '\\' so backslash is the escape char. */
static void ci_make_like_pattern(const char *id, int leading_pct, int trailing_pct, char *buf,
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

static const char *ci_find_sql = "SELECT p.name, f.path, t.line, t.kind"
                                 " FROM terms t"
                                 " JOIN files f ON f.id = t.file_id"
                                 " JOIN projects p ON p.id = f.project_id"
                                 " WHERE t.name = ?1"
                                 "   AND p.lifecycle_state = 'current'"
                                 "   AND f.generation = p.current_generation"
                                 "   AND (?3 = '' OR p.name = ?3)"
                                 "   AND f.path NOT LIKE '.%'"
                                 "   AND f.path NOT LIKE '%/.%'"
                                 "   AND p.root NOT LIKE '%/.%'"
                                 " GROUP BY p.name, f.path, t.line, t.kind"
                                 " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                                 " p.name, f.path"
                                 " LIMIT ?2";

static const char *ci_find_like_sql = "SELECT p.name, f.path, t.line, t.kind"
                                      " FROM terms t"
                                      " JOIN files f ON f.id = t.file_id"
                                      " JOIN projects p ON p.id = f.project_id"
                                      " WHERE t.name LIKE ?1 ESCAPE '\\'"
                                      "   AND p.lifecycle_state = 'current'"
                                      "   AND f.generation = p.current_generation"
                                      "   AND (?3 = '' OR p.name = ?3)"
                                      "   AND f.path NOT LIKE '.%'"
                                      "   AND f.path NOT LIKE '%/.%'"
                                      "   AND p.root NOT LIKE '%/.%'"
                                      " GROUP BY p.name, f.path, t.line, t.kind"
                                      " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                                      " p.name, f.path"
                                      " LIMIT ?2";

static const char *ci_find_excluding_sql =
    "SELECT p.name, f.path, t.line, t.kind"
    " FROM terms t"
    " JOIN files f ON f.id = t.file_id"
    " JOIN projects p ON p.id = f.project_id"
    " WHERE t.name = ?1"
    "   AND p.lifecycle_state = 'current'"
    "   AND f.generation = p.current_generation"
    "   AND p.name <> ?3"
    "   AND f.path NOT LIKE '.%'"
    "   AND f.path NOT LIKE '%/.%'"
    "   AND p.root NOT LIKE '%/.%'"
    " GROUP BY p.name, f.path, t.line, t.kind"
    " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
    " p.name, f.path"
    " LIMIT ?2";

static const char *ci_find_like_excluding_sql =
    "SELECT p.name, f.path, t.line, t.kind"
    " FROM terms t"
    " JOIN files f ON f.id = t.file_id"
    " JOIN projects p ON p.id = f.project_id"
    " WHERE t.name LIKE ?1 ESCAPE '\\'"
    "   AND p.lifecycle_state = 'current'"
    "   AND f.generation = p.current_generation"
    "   AND p.name <> ?3"
    "   AND f.path NOT LIKE '.%'"
    "   AND f.path NOT LIKE '%/.%'"
    "   AND p.root NOT LIKE '%/.%'"
    " GROUP BY p.name, f.path, t.line, t.kind"
    " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END, p.name, f.path"
    " LIMIT ?2";

static int ci_drain_term_hits(aimee_pg_stmt_t *st, term_hit_t *out, int max, char *err,
                              size_t errlen)
{
   int count = 0;
   while (count < max && aimee_pg_step(st, err, errlen) == AIMEE_PG_ROW)
   {
      const char *p = aimee_pg_column_text(st, 0);
      const char *f = aimee_pg_column_text(st, 1);
      int line = aimee_pg_column_int(st, 2);
      const char *k = aimee_pg_column_text(st, 3);
      snprintf(out[count].project, sizeof(out[count].project), "%s", p ? p : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f ? f : "");
      out[count].line = line;
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
      count++;
   }
   return count;
}

static int ci_find_scoped(const char *project, const char *excluded_project, const char *identifier,
                          term_hit_t *out, int max)
{
   if (project && project[0] && excluded_project && excluded_project[0])
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   const int excluding = excluded_project && excluded_project[0];
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, excluding ? ci_find_excluding_sql : ci_find_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identifier);
   aimee_pg_bind_int(st, "?2", max);
   aimee_pg_bind_text(st, "?3",
                      excluding ? excluded_project : (project && project[0] ? project : ""));
   int count = ci_drain_term_hits(st, out, max, err, sizeof(err));
   aimee_pg_finalize(st);

   /* Two LIKE fallbacks if exact match was empty: prefix first (preserves
    * "find aimee_db_t" precision), then substring (so "find qdrant" still
    * finds kb_test_qdrant_post_handler). Each tier only fires when the
    * previous one returned zero rows. */
   for (int tier = 0; count == 0 && tier < 2; tier++)
   {
      char pattern[512];
      ci_make_like_pattern(identifier, tier == 1, 1, pattern, sizeof(pattern));
      aimee_pg_stmt_t *st2 = aimee_pg_prepare(
          conn, excluding ? ci_find_like_excluding_sql : ci_find_like_sql, err, sizeof(err));
      if (!st2)
         break;
      aimee_pg_bind_text(st2, "?1", pattern);
      aimee_pg_bind_int(st2, "?2", max);
      aimee_pg_bind_text(st2, "?3",
                         excluding ? excluded_project : (project && project[0] ? project : ""));
      count = ci_drain_term_hits(st2, out, max, err, sizeof(err));
      aimee_pg_finalize(st2);
   }
   return count;
}

int canonical_index_find_project(const char *project, const char *identifier, term_hit_t *out,
                                 int max)
{
   return ci_find_scoped(project, NULL, identifier, out, max);
}

int canonical_index_find_excluding_project(const char *excluded_project, const char *identifier,
                                           term_hit_t *out, int max)
{
   if (!excluded_project || !excluded_project[0])
      return -1;
   return ci_find_scoped(NULL, excluded_project, identifier, out, max);
}

int canonical_index_find(const char *identifier, term_hit_t *out, int max)
{
   return canonical_index_find_project(NULL, identifier, out, max);
}

int canonical_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   if (!project || !file_path || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->file, sizeof(out->file), "%s", file_path);
   if (db2_code_index_blast_radius(project, file_path, out) != 0)
      return -1;

   /* Expand with co_edited graph edges (weight > 3): files that historically
    * change together but have no structural import edge. Basename-keyed legacy
    * projections are admitted only when they resolve to one current file. */
   {
      const char *slash = strrchr(file_path, '/');
      const char *match_name = slash ? slash + 1 : file_path;

      char co_buf[16][128];
      int n = db2_entity_edge_co_targets(match_name, "co_edited", 3, co_buf, 16);
      for (int b = 0; b < n && out->dependent_count < CI_MAX_DEPS; b++)
      {
         const char *related = co_buf[b];
         if (!related[0] || strcmp(related, match_name) == 0)
            continue;
         char resolved[MAX_PATH_LEN];
         if (db2_code_index_unique_file_basename(project, related, resolved, sizeof(resolved)) !=
                 1 ||
             strcmp(resolved, file_path) == 0)
            continue;
         int found = -1;
         for (int d = 0; d < out->dependent_count; d++)
         {
            if (strcmp(out->dependents[d], resolved) == 0 &&
                strcmp(out->dependent_meta[d].project, project) == 0)
            {
               found = d;
               break;
            }
         }
         if (found < 0)
         {
            if (out->dependent_count >= CI_MAX_DEPS)
               continue;
            found = out->dependent_count++;
            snprintf(out->dependents[found], MAX_PATH_LEN, "%s", resolved);
            snprintf(out->dependent_meta[found].project, sizeof(out->dependent_meta[found].project),
                     "%s", project);
            out->dependent_meta[found].generation = out->generation;
            snprintf(out->dependent_meta[found].freshness,
                     sizeof(out->dependent_meta[found].freshness), "current");
            snprintf(out->dependent_meta[found].confidence,
                     sizeof(out->dependent_meta[found].confidence), "low");
         }
         char *provenance = out->dependent_meta[found].provenance;
         if (provenance[0] && !strstr(provenance, "projection"))
            strncat(provenance, ",projection",
                    sizeof(out->dependent_meta[found].provenance) - strlen(provenance) - 1);
         else if (!provenance[0])
            snprintf(provenance, sizeof(out->dependent_meta[found].provenance), "projection");
      }
   }

   db2_code_index_blast_radius_local_first(project, out);

   return 0;
}

int canonical_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max)
{
   void *conn = ci_conn();
   if (!conn)
      return -1;

   int64_t project_id = ci_resolve_project_id(conn, project);
   if (project_id < 0)
      return 0;
   int64_t file_id = ci_resolve_file_id(conn, project_id, file_path);
   if (file_id < 0)
      return 0;

   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT name, kind, line FROM terms"
                                          " WHERE file_id = ?1 AND kind = 'definition'"
                                          " ORDER BY line",
                                          err, sizeof(err));
   if (!st)
      return 0;
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
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

static int ci_find_callers_scoped(const char *project, const char *excluded_project,
                                  const char *symbol, caller_hit_t *out, int max)
{
   if (project && project[0] && excluded_project && excluded_project[0])
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   const char *sql_all = "SELECT p.name, f.path, cc.caller, cc.line"
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
   const char *sql_proj = "SELECT p.name, f.path, cc.caller, cc.line"
                          " FROM code_calls cc"
                          " JOIN files f ON f.id = cc.file_id"
                          " JOIN projects p ON p.id = f.project_id"
                          " WHERE cc.callee = ?1 AND p.name = ?2"
                          "   AND p.lifecycle_state = 'current'"
                          "   AND f.generation = p.current_generation"
                          "   AND f.path NOT LIKE '.%'"
                          "   AND f.path NOT LIKE '%/.%'"
                          "   AND p.root NOT LIKE '%/.%'"
                          " ORDER BY f.path, cc.line"
                          " LIMIT ?3";
   const char *sql_excluding = "SELECT p.name, f.path, cc.caller, cc.line"
                               " FROM code_calls cc"
                               " JOIN files f ON f.id = cc.file_id"
                               " JOIN projects p ON p.id = f.project_id"
                               " WHERE cc.callee = ?1 AND p.name <> ?2"
                               "   AND p.lifecycle_state = 'current'"
                               "   AND f.generation = p.current_generation"
                               "   AND f.path NOT LIKE '.%'"
                               "   AND f.path NOT LIKE '%/.%'"
                               "   AND p.root NOT LIKE '%/.%'"
                               " ORDER BY p.name, f.path, cc.line"
                               " LIMIT ?3";

   aimee_pg_stmt_t *st;
   if (excluded_project && excluded_project[0])
   {
      st = aimee_pg_prepare(conn, sql_excluding, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", symbol);
      aimee_pg_bind_text(st, "?2", excluded_project);
      aimee_pg_bind_int(st, "?3", max);
   }
   else if (project && project[0])
   {
      st = aimee_pg_prepare(conn, sql_proj, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", symbol);
      aimee_pg_bind_text(st, "?2", project);
      aimee_pg_bind_int(st, "?3", max);
   }
   else
   {
      st = aimee_pg_prepare(conn, sql_all, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", symbol);
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

int canonical_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max)
{
   return ci_find_callers_scoped(project, NULL, symbol, out, max);
}

int canonical_index_find_callers_excluding_project(const char *excluded_project, const char *symbol,
                                                   caller_hit_t *out, int max)
{
   if (!excluded_project || !excluded_project[0])
      return -1;
   return ci_find_callers_scoped(NULL, excluded_project, symbol, out, max);
}

int canonical_index_project_stats(const char *project, int *files_out, int *defs_out)
{
   if (files_out)
      *files_out = 0;
   if (defs_out)
      *defs_out = 0;
   if (!project || !project[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT"
                            " (SELECT COUNT(*) FROM files f"
                            "  JOIN projects p ON p.id = f.project_id"
                            "  WHERE p.name = ?1"
                            "    AND p.lifecycle_state = 'current'"
                            "    AND f.generation = p.current_generation"
                            "    AND f.path NOT LIKE '.%'"
                            "    AND f.path NOT LIKE '%/.%'"
                            "    AND p.root NOT LIKE '%/.%'),"
                            " (SELECT COUNT(*) FROM terms t"
                            "  JOIN files f ON f.id = t.file_id"
                            "  JOIN projects p ON p.id = f.project_id"
                            "  WHERE p.name = ?2 AND t.kind = 'definition'"
                            "    AND p.lifecycle_state = 'current'"
                            "    AND f.generation = p.current_generation"
                            "    AND f.path NOT LIKE '.%'"
                            "    AND f.path NOT LIKE '%/.%'"
                            "    AND p.root NOT LIKE '%/.%')";
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", project);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (files_out)
         *files_out = aimee_pg_column_int(st, 0);
      if (defs_out)
         *defs_out = aimee_pg_column_int(st, 1);
   }
   aimee_pg_finalize(st);
   return 0;
}

int canonical_index_project_lang_breakdown(const char *project, char *buf, size_t bufsz)
{
   if (!buf || bufsz < 3)
      return -1;
   buf[0] = '[';
   buf[1] = ']';
   buf[2] = '\0';
   if (!project || !project[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT COALESCE(lower(substring(f.path FROM '\\.([^./]+)$')), '') AS ext,"
       "       COUNT(*) AS cnt"
       " FROM files f"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE p.name = ?1"
       "   AND p.lifecycle_state = 'current'"
       "   AND f.generation = p.current_generation"
       "   AND f.path NOT LIKE '%/'"
       "   AND f.path NOT LIKE '.%'"
       "   AND f.path NOT LIKE '%/.%'"
       "   AND p.root NOT LIKE '%/.%'"
       " GROUP BY ext"
       " ORDER BY cnt DESC"
       " LIMIT 8";
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);

   size_t pos = 0;
   /* Write into a temporary buffer and then copy into buf. */
   char tmp[1024];
   tmp[pos++] = '[';

   int first = 1;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ext = aimee_pg_column_text(st, 0);
      int cnt = aimee_pg_column_int(st, 1);
      if (!ext || !ext[0])
         ext = "other";
      int n = snprintf(tmp + pos, sizeof(tmp) - pos, "%s{\"lang\":\"%s\",\"count\":%d}",
                       first ? "" : ",", ext, cnt);
      if (n > 0 && (size_t)n < sizeof(tmp) - pos)
         pos += (size_t)n;
      first = 0;
      if (pos + 32 >= sizeof(tmp))
         break;
   }
   aimee_pg_finalize(st);

   if (pos + 2 > sizeof(tmp))
      pos = sizeof(tmp) - 2;
   tmp[pos++] = ']';
   tmp[pos] = '\0';

   snprintf(buf, bufsz, "%s", tmp);
   return 0;
}

int canonical_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max, int enrich)
{
   /* Forward to db2_code_index — same SQL, same connection. */
   return db2_code_index_code_search(query, project, out, max, enrich);
}

int canonical_index_code_search_excluding_project(const char *query, const char *excluded_project,
                                                  code_search_hit_t *out, int max, int enrich)
{
   return db2_code_index_code_search_excluding_project(query, excluded_project, out, max, enrich);
}
