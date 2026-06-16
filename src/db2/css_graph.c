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
