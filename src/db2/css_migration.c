/* db2/css_migration.c: CSS migration pipeline driver. See css_migration.h. */
#include "css_migration.h"

#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSSM_ERRBUF 256

css_migration_gate_t css_migration_gate(int resolved_tokens, int total_tokens,
                                        int oracle_equivalent, int coverage_threshold_pct)
{
   if (oracle_equivalent != 1)
      return CSS_MIGRATION_GATE_NEEDS_REVIEW;
   if (coverage_threshold_pct < 0)
      coverage_threshold_pct = 0;
   if (coverage_threshold_pct > 100)
      coverage_threshold_pct = 100;
   /* No tokens at all -> nothing to resolve; treat as full coverage. */
   int pct = total_tokens <= 0 ? 100 : (int)((long)resolved_tokens * 100 / total_tokens);
   return pct >= coverage_threshold_pct ? CSS_MIGRATION_GATE_AUTO_ACCEPT
                                        : CSS_MIGRATION_GATE_NEEDS_REVIEW;
}

int db2_css_migration_enumerate(const char *project)
{
   if (!project || !project[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* One unit per component file that has class tokens, with coverage. Upsert so
    * a re-enumerate refreshes coverage without resetting an in-flight state. */
   static const char *sql =
       "SELECT f.path, COUNT(*) AS total, SUM(CASE WHEN cs.resolved = 1 THEN 1 ELSE 0 END) AS res"
       " FROM css_component_styles cs"
       " JOIN files f ON f.id = cs.component_file_id"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE p.name = ?1"
       " GROUP BY f.path";
   char err[CSSM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);

   /* Collect rows first (can't run nested writes while iterating one stmt).
    * Heap-allocated — MAX_PATH_LEN rows would overflow the stack. */
   typedef struct
   {
      char path[MAX_PATH_LEN];
      int total, res;
   } row_t;
   enum
   {
      ROW_CAP = 4096
   };
   row_t *rows = malloc((size_t)ROW_CAP * sizeof(row_t));
   if (!rows)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   int nrows = 0;
   while (nrows < ROW_CAP && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *path = aimee_pg_column_text(st, 0);
      snprintf(rows[nrows].path, sizeof(rows[nrows].path), "%s", path ? path : "");
      rows[nrows].total = aimee_pg_column_int(st, 1);
      rows[nrows].res = aimee_pg_column_int(st, 2);
      nrows++;
   }
   aimee_pg_finalize(st);

   int rc = 0;
   for (int i = 0; i < nrows && rc == 0; i++)
   {
      static const char *up = "INSERT INTO css_migration_units (project, unit_path, state, "
                              "total_tokens, resolved_tokens)"
                              " VALUES (?1, ?2, 'pending', ?3, ?4)"
                              " ON CONFLICT(project, unit_path)"
                              " DO UPDATE SET total_tokens = ?3, resolved_tokens = ?4";
      aimee_pg_stmt_t *su = aimee_pg_prepare(conn, up, err, sizeof(err));
      if (!su)
      {
         rc = -1;
         break;
      }
      aimee_pg_bind_text(su, "?1", project);
      aimee_pg_bind_text(su, "?2", rows[i].path);
      aimee_pg_bind_int(su, "?3", rows[i].total);
      aimee_pg_bind_int(su, "?4", rows[i].res);
      if (aimee_pg_step(su, err, sizeof(err)) != AIMEE_PG_DONE)
         rc = -1;
      aimee_pg_finalize(su);
   }
   free(rows);
   return rc == 0 ? nrows : -1;
}

int db2_css_migration_set_state(const char *project, const char *unit_path, const char *state,
                                int oracle_equivalent, const char *note, const char *now_iso)
{
   if (!project || !unit_path || !state)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "UPDATE css_migration_units"
                            " SET state = ?3, oracle_equivalent = ?4, note = ?5, updated_at = ?6"
                            " WHERE project = ?1 AND unit_path = ?2";
   char err[CSSM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", unit_path);
   aimee_pg_bind_text(st, "?3", state);
   aimee_pg_bind_int(st, "?4", oracle_equivalent);
   aimee_pg_bind_text(st, "?5", note ? note : "");
   aimee_pg_bind_text(st, "?6", now_iso ? now_iso : "");
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_css_migration_list(const char *project, const char *state_filter, css_migration_unit_t *out,
                           int max)
{
   if (!project || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (state_filter && state_filter[0]) ? 1 : 0;
   static const char *sql_all =
       "SELECT unit_path, state, total_tokens, resolved_tokens, oracle_equivalent, note"
       " FROM css_migration_units WHERE project = ?1 ORDER BY unit_path LIMIT ?2";
   static const char *sql_filt =
       "SELECT unit_path, state, total_tokens, resolved_tokens, oracle_equivalent, note"
       " FROM css_migration_units WHERE project = ?1 AND state = ?3 ORDER BY unit_path LIMIT ?2";
   char err[CSSM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? sql_filt : sql_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_int(st, "?2", max);
   if (filt)
      aimee_pg_bind_text(st, "?3", state_filter);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_migration_unit_t *u = &out[count];
      memset(u, 0, sizeof(*u));
      const char *up = aimee_pg_column_text(st, 0);
      const char *stt = aimee_pg_column_text(st, 1);
      const char *nt = aimee_pg_column_text(st, 5);
      snprintf(u->unit_path, sizeof(u->unit_path), "%s", up ? up : "");
      snprintf(u->state, sizeof(u->state), "%s", stt ? stt : "");
      u->total_tokens = aimee_pg_column_int(st, 2);
      u->resolved_tokens = aimee_pg_column_int(st, 3);
      u->oracle_equivalent = aimee_pg_column_int(st, 4);
      snprintf(u->note, sizeof(u->note), "%s", nt ? nt : "");
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

/* Count rows for a single-bind-text query (returns -1 on error). */
static int cssm_count(void *conn, const char *sql, const char *p1)
{
   char err[CSSM_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", p1);
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_css_migration_rules_doc(const char *exemplar_project, char *buf, size_t cap)
{
   if (!exemplar_project || !buf || cap == 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   int rules = cssm_count(conn,
                          "SELECT COUNT(*) FROM css_rules c JOIN files f ON f.id = c.file_id"
                          " JOIN projects p ON p.id = f.project_id WHERE p.name = ?1",
                          exemplar_project);
   int tokens =
       cssm_count(conn,
                  "SELECT COUNT(*) FROM css_declarations d JOIN css_rules c ON c.id = d.rule_id"
                  " JOIN files f ON f.id = c.file_id JOIN projects p ON p.id = f.project_id"
                  " WHERE p.name = ?1 AND d.property LIKE '--%'",
                  exemplar_project);
   /* BEM heuristic: presence of element/modifier delimiters in class selectors. */
   int bem = cssm_count(conn,
                        "SELECT COUNT(*) FROM css_rules c JOIN files f ON f.id = c.file_id"
                        " JOIN projects p ON p.id = f.project_id"
                        " WHERE p.name = ?1 AND (c.selector LIKE '%\\_\\_%' ESCAPE '\\'"
                        "   OR c.selector LIKE '%--%')",
                        exemplar_project);
   if (rules < 0)
      return -1;
   const char *naming = bem > 0 ? "BEM-like (block__element--modifier delimiters present)"
                                : "flat / utility (no BEM delimiters detected)";

   int n = snprintf(buf, cap,
                    "# CSS Migration — Convention Rules (derived from exemplar `%s`)\n\n"
                    "> Degraded #2 convention model: a written rules document derived from the\n"
                    "> exemplar's indexed style graph. A human (or delegate) CONFIRMS these\n"
                    "> before they gate any conversion. No typed-fact dependency.\n\n"
                    "## Derived signals\n"
                    "- Indexed rules in exemplar: **%d**\n"
                    "- Custom-property (design token) declarations: **%d**\n"
                    "- Detected naming convention: **%s**\n\n"
                    "## Conventions each conversion must satisfy (confirm/edit)\n"
                    "1. **File layout:** a component owns its styles (co-located stylesheet or\n"
                    "   module), mirroring the exemplar's structure.\n"
                    "2. **Naming:** follow the detected scheme above for new class names.\n"
                    "3. **Tokens:** reuse the exemplar's custom properties; do not hard-code\n"
                    "   values that a token already covers.\n"
                    "4. **No Tailwind utilities in output:** utilities are expanded to plain,\n"
                    "   structured CSS modelled on the exemplar.\n"
                    "5. **Equivalence:** the WP-E static oracle must report the converted\n"
                    "   unit's resolved declaration set as equivalent (or the diff is\n"
                    "   explained and approved).\n\n"
                    "_Verify these against the exemplar before fan-out; pilot one component\n"
                    "end-to-end first._\n",
                    exemplar_project, rules, tokens < 0 ? 0 : tokens, naming);
   if (n < 0)
      return -1;
   return n;
}
