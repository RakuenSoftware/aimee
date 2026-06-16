/* db2/css_graph.c: CSS style-graph persistence. See css_graph.h. */
#include "css_graph.h"

#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CSSG_ERRBUF 256

int64_t db2_css_graph_resolve_file(const char *project, const char *file_path)
{
   if (!project || !file_path)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT f.id FROM files f"
                            " JOIN projects p ON p.id = f.project_id"
                            " WHERE p.name = ?1 AND f.path = ?2";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_css_graph_replace(int64_t file_id, const css_rule_t *rules, int n)
{
   if (file_id < 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CSSG_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   int rc = 0;
   /* css_declarations cascade off css_rules, so one delete clears the file. */
   if (db2_exec_conn_int64(conn, "DELETE FROM css_rules WHERE file_id = ?1", file_id) != 0)
      rc = -1;

   for (int i = 0; rc == 0 && i < n && rules; i++)
   {
      const css_rule_t *r = &rules[i];
      static const char *ins_rule =
          "INSERT INTO css_rules"
          " (file_id, selector, spec_a, spec_b, spec_c, spec_uncertain, at_context, line)"
          " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) RETURNING id";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins_rule, err, sizeof(err));
      if (!st)
      {
         rc = -1;
         break;
      }
      aimee_pg_bind_int64(st, "?1", file_id);
      aimee_pg_bind_text(st, "?2", r->selector);
      aimee_pg_bind_int(st, "?3", r->spec_a);
      aimee_pg_bind_int(st, "?4", r->spec_b);
      aimee_pg_bind_int(st, "?5", r->spec_c);
      aimee_pg_bind_int(st, "?6", r->specificity_uncertain ? 1 : 0);
      aimee_pg_bind_text(st, "?7", r->at_context);
      aimee_pg_bind_int(st, "?8", r->line);
      int64_t rule_id = -1;
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         rule_id = aimee_pg_column_int64(st, 0);
      else
         rc = -1;
      aimee_pg_finalize(st);
      if (rc != 0 || rule_id < 0)
      {
         rc = -1;
         break;
      }

      for (int d = 0; rc == 0 && d < r->decl_count && r->decls; d++)
      {
         static const char *ins_decl =
             "INSERT INTO css_declarations (rule_id, property, value, important)"
             " VALUES (?1, ?2, ?3, ?4)";
         aimee_pg_stmt_t *sd = aimee_pg_prepare(conn, ins_decl, err, sizeof(err));
         if (!sd)
         {
            rc = -1;
            break;
         }
         aimee_pg_bind_int64(sd, "?1", rule_id);
         aimee_pg_bind_text(sd, "?2", r->decls[d].property);
         aimee_pg_bind_text(sd, "?3", r->decls[d].value);
         aimee_pg_bind_int(sd, "?4", r->decls[d].important ? 1 : 0);
         if (aimee_pg_step(sd, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = -1;
         aimee_pg_finalize(sd);
      }
   }

   if (rc == 0)
      aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
   else
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc;
}

int db2_css_graph_upsert_file(const char *project, const char *file_path, const css_rule_t *rules,
                              int n)
{
   int64_t file_id = db2_css_graph_resolve_file(project, file_path);
   if (file_id < 0)
      return -1;
   return db2_css_graph_replace(file_id, rules, n);
}

int db2_css_graph_rules_by_selector(const char *selector, css_rule_hit_t *out, int max)
{
   if (!selector || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT p.name, f.path, c.selector, c.spec_a, c.spec_b, c.spec_c,"
                            "       c.spec_uncertain, c.at_context, c.line"
                            " FROM css_rules c"
                            " JOIN files f ON f.id = c.file_id"
                            " JOIN projects p ON p.id = f.project_id"
                            " WHERE c.selector = ?1"
                            " ORDER BY p.name, f.path, c.line"
                            " LIMIT ?2";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", selector);
   aimee_pg_bind_int(st, "?2", max);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_rule_hit_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *sel = aimee_pg_column_text(st, 2);
      const char *atc = aimee_pg_column_text(st, 7);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->selector, sizeof(h->selector), "%s", sel ? sel : "");
      h->spec_a = aimee_pg_column_int(st, 3);
      h->spec_b = aimee_pg_column_int(st, 4);
      h->spec_c = aimee_pg_column_int(st, 5);
      h->spec_uncertain = aimee_pg_column_int(st, 6);
      snprintf(h->at_context, sizeof(h->at_context), "%s", atc ? atc : "");
      h->line = aimee_pg_column_int(st, 8);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_css_graph_declarations_by_property(const char *property, css_decl_hit_t *out, int max)
{
   if (!property || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT p.name, f.path, c.selector, d.property, d.value, d.important"
                            " FROM css_declarations d"
                            " JOIN css_rules c ON c.id = d.rule_id"
                            " JOIN files f ON f.id = c.file_id"
                            " JOIN projects p ON p.id = f.project_id"
                            " WHERE d.property = ?1"
                            " ORDER BY p.name, f.path, c.line"
                            " LIMIT ?2";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", property);
   aimee_pg_bind_int(st, "?2", max);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_decl_hit_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *sel = aimee_pg_column_text(st, 2);
      const char *prop = aimee_pg_column_text(st, 3);
      const char *val = aimee_pg_column_text(st, 4);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->selector, sizeof(h->selector), "%s", sel ? sel : "");
      snprintf(h->property, sizeof(h->property), "%s", prop ? prop : "");
      snprintf(h->value, sizeof(h->value), "%s", val ? val : "");
      h->important = aimee_pg_column_int(st, 5);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

/* Specificity encoded as a single comparable integer for cascade ordering. The
 * components are tiny in practice; the multipliers keep them lexicographic. */
#define CSS_SPEC_SQL(pfx) "(" pfx ".spec_a*1000000 + " pfx ".spec_b*1000 + " pfx ".spec_c)"

int db2_css_graph_duplicate_declarations(const char *project_filter, css_dup_decl_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (project_filter && project_filter[0]) ? 1 : 0;
   static const char *sql_all = "SELECT p.name, f.path, d.property, d.value, COUNT(*) AS cnt"
                                " FROM css_declarations d"
                                " JOIN css_rules c ON c.id = d.rule_id"
                                " JOIN files f ON f.id = c.file_id"
                                " JOIN projects p ON p.id = f.project_id"
                                " GROUP BY f.id, d.property, d.value"
                                " HAVING COUNT(*) > 1"
                                " ORDER BY cnt DESC, f.path"
                                " LIMIT ?1";
   static const char *sql_filt = "SELECT p.name, f.path, d.property, d.value, COUNT(*) AS cnt"
                                 " FROM css_declarations d"
                                 " JOIN css_rules c ON c.id = d.rule_id"
                                 " JOIN files f ON f.id = c.file_id"
                                 " JOIN projects p ON p.id = f.project_id"
                                 " WHERE p.name = ?2"
                                 " GROUP BY f.id, d.property, d.value"
                                 " HAVING COUNT(*) > 1"
                                 " ORDER BY cnt DESC, f.path"
                                 " LIMIT ?1";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? sql_filt : sql_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   if (filt)
      aimee_pg_bind_text(st, "?2", project_filter);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_dup_decl_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *prop = aimee_pg_column_text(st, 2);
      const char *val = aimee_pg_column_text(st, 3);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->property, sizeof(h->property), "%s", prop ? prop : "");
      snprintf(h->value, sizeof(h->value), "%s", val ? val : "");
      h->count = aimee_pg_column_int(st, 4);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_css_graph_duplicate_selectors(const char *project_filter, css_dup_selector_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (project_filter && project_filter[0]) ? 1 : 0;
   static const char *sql_all = "SELECT p.name, f.path, c.selector, COUNT(*) AS cnt"
                                " FROM css_rules c"
                                " JOIN files f ON f.id = c.file_id"
                                " JOIN projects p ON p.id = f.project_id"
                                " GROUP BY f.id, c.selector"
                                " HAVING COUNT(*) > 1"
                                " ORDER BY cnt DESC, f.path"
                                " LIMIT ?1";
   static const char *sql_filt = "SELECT p.name, f.path, c.selector, COUNT(*) AS cnt"
                                 " FROM css_rules c"
                                 " JOIN files f ON f.id = c.file_id"
                                 " JOIN projects p ON p.id = f.project_id"
                                 " WHERE p.name = ?2"
                                 " GROUP BY f.id, c.selector"
                                 " HAVING COUNT(*) > 1"
                                 " ORDER BY cnt DESC, f.path"
                                 " LIMIT ?1";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? sql_filt : sql_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   if (filt)
      aimee_pg_bind_text(st, "?2", project_filter);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_dup_selector_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *sel = aimee_pg_column_text(st, 2);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->selector, sizeof(h->selector), "%s", sel ? sel : "");
      h->count = aimee_pg_column_int(st, 3);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_css_graph_specificity_conflicts(const char *project_filter, css_spec_conflict_t *out,
                                        int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (project_filter && project_filter[0]) ? 1 : 0;
   /* c1 = earlier + strictly more specific (the winner); c2 = the later rule the
    * author likely expected to apply but which is overridden for `property`. */
   static const char *sql_all =
       "SELECT p.name, f.path, d1.property, c1.selector, c1.line, c2.selector, c2.line"
       " FROM css_declarations d1"
       " JOIN css_rules c1 ON c1.id = d1.rule_id"
       " JOIN css_declarations d2 ON d2.property = d1.property"
       " JOIN css_rules c2 ON c2.id = d2.rule_id"
       " JOIN files f ON f.id = c1.file_id"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE c1.file_id = c2.file_id AND c1.id <> c2.id"
       "   AND c2.line > c1.line"
       "   AND c1.spec_uncertain = 0 AND c2.spec_uncertain = 0"
       "   AND c1.selector <> c2.selector"
       "   AND " CSS_SPEC_SQL("c1") " > " CSS_SPEC_SQL("c2") " ORDER BY f.path, c2.line LIMIT ?1";
   static const char *sql_filt =
       "SELECT p.name, f.path, d1.property, c1.selector, c1.line, c2.selector, c2.line"
       " FROM css_declarations d1"
       " JOIN css_rules c1 ON c1.id = d1.rule_id"
       " JOIN css_declarations d2 ON d2.property = d1.property"
       " JOIN css_rules c2 ON c2.id = d2.rule_id"
       " JOIN files f ON f.id = c1.file_id"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE c1.file_id = c2.file_id AND c1.id <> c2.id"
       "   AND c2.line > c1.line"
       "   AND c1.spec_uncertain = 0 AND c2.spec_uncertain = 0"
       "   AND c1.selector <> c2.selector"
       "   AND " CSS_SPEC_SQL("c1") " > " CSS_SPEC_SQL("c2") " AND p.name = ?2"
                                                             " ORDER BY f.path, c2.line LIMIT ?1";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? sql_filt : sql_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   if (filt)
      aimee_pg_bind_text(st, "?2", project_filter);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_spec_conflict_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *prop = aimee_pg_column_text(st, 2);
      const char *ws = aimee_pg_column_text(st, 3);
      const char *ls = aimee_pg_column_text(st, 5);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->property, sizeof(h->property), "%s", prop ? prop : "");
      snprintf(h->winner_selector, sizeof(h->winner_selector), "%s", ws ? ws : "");
      h->winner_line = aimee_pg_column_int(st, 4);
      snprintf(h->loser_selector, sizeof(h->loser_selector), "%s", ls ? ls : "");
      h->loser_line = aimee_pg_column_int(st, 6);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

/* ---- component <-> style join (WP-D) ----------------------------------- */

int db2_css_component_resolve(int64_t component_file_id, const char (*tokens)[CSS_CLASS_TOKEN_MAX],
                              int n)
{
   if (component_file_id < 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CSSG_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   int rc = 0;
   if (db2_exec_conn_int64(conn, "DELETE FROM css_component_styles WHERE component_file_id = ?1",
                           component_file_id) != 0)
      rc = -1;

   for (int i = 0; rc == 0 && i < n && tokens; i++)
   {
      if (!tokens[i][0])
         continue;
      char sel[CSS_SELECTOR_MAX];
      snprintf(sel, sizeof(sel), ".%s", tokens[i]);

      /* match a simple class rule of the same selector, anywhere in the index */
      int64_t rule_id = -1;
      static const char *q = "SELECT id FROM css_rules WHERE selector = ?1 LIMIT 1";
      aimee_pg_stmt_t *sq = aimee_pg_prepare(conn, q, err, sizeof(err));
      if (!sq)
      {
         rc = -1;
         break;
      }
      aimee_pg_bind_text(sq, "?1", sel);
      if (aimee_pg_step(sq, err, sizeof(err)) == AIMEE_PG_ROW)
         rule_id = aimee_pg_column_int64(sq, 0);
      aimee_pg_finalize(sq);

      static const char *ins =
          "INSERT INTO css_component_styles (component_file_id, class_token, rule_id, resolved)"
          " VALUES (?1, ?2, ?3, ?4)";
      aimee_pg_stmt_t *si = aimee_pg_prepare(conn, ins, err, sizeof(err));
      if (!si)
      {
         rc = -1;
         break;
      }
      aimee_pg_bind_int64(si, "?1", component_file_id);
      aimee_pg_bind_text(si, "?2", tokens[i]);
      aimee_pg_bind_int64(si, "?3", rule_id);
      aimee_pg_bind_int(si, "?4", rule_id >= 0 ? 1 : 0);
      if (aimee_pg_step(si, err, sizeof(err)) != AIMEE_PG_DONE)
         rc = -1;
      aimee_pg_finalize(si);
   }

   if (rc == 0)
      aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
   else
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc;
}

int db2_css_component_unresolved(const char *project_filter, css_unresolved_hit_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (project_filter && project_filter[0]) ? 1 : 0;
   static const char *sql_all = "SELECT DISTINCT p.name, f.path, cs.class_token"
                                " FROM css_component_styles cs"
                                " JOIN files f ON f.id = cs.component_file_id"
                                " JOIN projects p ON p.id = f.project_id"
                                " WHERE cs.resolved = 0"
                                " ORDER BY f.path, cs.class_token LIMIT ?1";
   static const char *sql_filt = "SELECT DISTINCT p.name, f.path, cs.class_token"
                                 " FROM css_component_styles cs"
                                 " JOIN files f ON f.id = cs.component_file_id"
                                 " JOIN projects p ON p.id = f.project_id"
                                 " WHERE cs.resolved = 0 AND p.name = ?2"
                                 " ORDER BY f.path, cs.class_token LIMIT ?1";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? sql_filt : sql_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   if (filt)
      aimee_pg_bind_text(st, "?2", project_filter);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_unresolved_hit_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *tk = aimee_pg_column_text(st, 2);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->class_token, sizeof(h->class_token), "%s", tk ? tk : "");
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

/* Portable (LIKE-only, works on Postgres + the sqlite shim) predicate that a
 * selector is a single simple class (".ident", no combinator/compound/second
 * class), plus a NOT EXISTS over resolved component references in the project. */
#define CSS_DEAD_RULE_WHERE                                                                        \
   " c.selector LIKE '.%' AND c.selector NOT LIKE '% %' AND c.selector NOT LIKE '%>%'"             \
   " AND c.selector NOT LIKE '%+%' AND c.selector NOT LIKE '%~%' AND c.selector NOT LIKE '%:%'"    \
   " AND c.selector NOT LIKE '%[%' AND c.selector NOT LIKE '%#%' AND c.selector NOT LIKE '%*%'"    \
   " AND c.selector NOT LIKE '%,%' AND substr(c.selector, 2) NOT LIKE '%.%'"                       \
   " AND c.spec_uncertain = 0"                                                                     \
   " AND NOT EXISTS (SELECT 1 FROM css_component_styles cs JOIN files cf"                          \
   "   ON cf.id = cs.component_file_id WHERE cf.project_id = f.project_id AND cs.resolved = 1"     \
   "   AND ('.' || cs.class_token) = c.selector)"

int db2_css_dead_rules(const char *project_filter, css_dead_rule_hit_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (project_filter && project_filter[0]) ? 1 : 0;
   static const char *sql_all = "SELECT p.name, f.path, c.selector, c.line"
                                " FROM css_rules c"
                                " JOIN files f ON f.id = c.file_id"
                                " JOIN projects p ON p.id = f.project_id"
                                " WHERE" CSS_DEAD_RULE_WHERE " ORDER BY f.path, c.line LIMIT ?1";
   static const char *sql_filt =
       "SELECT p.name, f.path, c.selector, c.line"
       " FROM css_rules c"
       " JOIN files f ON f.id = c.file_id"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE p.name = ?2 AND" CSS_DEAD_RULE_WHERE " ORDER BY f.path, c.line LIMIT ?1";
   char err[CSSG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? sql_filt : sql_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   if (filt)
      aimee_pg_bind_text(st, "?2", project_filter);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_dead_rule_hit_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *sel = aimee_pg_column_text(st, 2);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->selector, sizeof(h->selector), "%s", sel ? sel : "");
      h->line = aimee_pg_column_int(st, 3);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}
