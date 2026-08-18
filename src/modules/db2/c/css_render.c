/* db2/css_render.c: storage + evaluation for the rendered computed-style oracle.
 * See css_render.h. */
#include "css_render.h"

#include "../support/db2_runtime_config.h"
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSSR_ERRBUF 256

static db2_css_render_compare_fn css_render_compare_provider;

void aimee_db2_register_css_render_compare_provider(db2_css_render_compare_fn provider)
{
   css_render_compare_provider = provider;
}

int db2_css_render_compare(const char *before_json, const char *after_json, int *before_valid,
                           int *after_valid, int *available, int *equivalent, int *diff_count)
{
   if (!before_valid || !after_valid || !available || !equivalent || !diff_count)
      return -1;
   *before_valid = 0;
   *after_valid = 0;
   *available = 0;
   *equivalent = 0;
   *diff_count = 0;
   if (!css_render_compare_provider ||
       css_render_compare_provider(before_json, after_json, before_valid, after_valid, available,
                                   equivalent, diff_count) != 0 ||
       (*before_valid != 0 && *before_valid != 1) || (*after_valid != 0 && *after_valid != 1) ||
       (*available != 0 && *available != 1) || (*equivalent != 0 && *equivalent != 1) ||
       *diff_count < 0 || *available != (*before_valid && *after_valid) ||
       (!*available && (*equivalent || *diff_count)) ||
       (*available && *equivalent != (*diff_count == 0)))
   {
      *before_valid = 0;
      *after_valid = 0;
      *available = 0;
      *equivalent = 0;
      *diff_count = 0;
      return -1;
   }
   return 0;
}

static int cssr_enabled(void)
{
   /* config_css_style_graph_enabled() fails closed on a load error, so the
    * explicit load-and-check this replaced is redundant. */
   return config_css_style_graph_enabled() ? 1 : 0;
}

static int phase_valid(const char *phase)
{
   return phase && (strcmp(phase, "before") == 0 || strcmp(phase, "after") == 0);
}

/* FNV-1a 64-bit -> 16 hex chars, for change-detection / audit of a snapshot. */
static void cssr_hash(const char *s, char out[17])
{
   uint64_t h = 1469598103934665603ULL;
   for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++)
   {
      h ^= *p;
      h *= 1099511628211ULL;
   }
   snprintf(out, 17, "%016llx", (unsigned long long)h);
}

int db2_css_render_snapshot_store(const char *project, const char *unit_path, const char *phase,
                                  const char *snapshot_json, const char *now_iso)
{
   if (!project || !project[0] || !unit_path || !unit_path[0] || !snapshot_json)
      return -1;
   if (!phase_valid(phase))
      return -1;
   if (!cssr_enabled())
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char hash[17];
   cssr_hash(snapshot_json, hash);
   char err[CSSR_ERRBUF] = "";

   /* delete-then-insert upsert on (project, unit_path, phase). */
   static const char *del = "DELETE FROM css_render_snapshots"
                            " WHERE project = ?1 AND unit_path = ?2 AND phase = ?3"
                            " AND generation=(SELECT current_generation FROM projects p"
                            " WHERE p.name=?1 AND p.lifecycle_state='current')";
   aimee_pg_stmt_t *dst = aimee_pg_prepare(conn, del, err, sizeof(err));
   if (!dst)
      return -1;
   aimee_pg_bind_text(dst, "?1", project);
   aimee_pg_bind_text(dst, "?2", unit_path);
   aimee_pg_bind_text(dst, "?3", phase);
   int ok = (aimee_pg_step(dst, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(dst);
   if (!ok)
      return -1;

   static const char *ins = "INSERT INTO css_render_snapshots"
                            " (project, generation, unit_path, phase, snapshot, content_hash,"
                            " captured_at)"
                            " SELECT ?1, p.current_generation, ?2, ?3, ?4, ?5, ?6"
                            " FROM projects p WHERE p.name=?1 AND p.lifecycle_state='current'";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", unit_path);
   aimee_pg_bind_text(st, "?3", phase);
   aimee_pg_bind_text(st, "?4", snapshot_json);
   aimee_pg_bind_text(st, "?5", hash);
   aimee_pg_bind_text(st, "?6", now_iso ? now_iso : "");
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE && aimee_pg_stmt_changes(st) == 1)
                ? 1
                : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_css_render_snapshot_get(const char *project, const char *unit_path, const char *phase,
                                char **out)
{
   if (out)
      *out = NULL;
   if (!project || !unit_path || !phase_valid(phase) || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT s.snapshot FROM css_render_snapshots s"
                            " JOIN projects p ON p.name=s.project"
                            " WHERE s.project = ?1 AND s.unit_path = ?2 AND s.phase = ?3"
                            " AND p.lifecycle_state='current'"
                            " AND s.generation=p.current_generation";
   char err[CSSR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", unit_path);
   aimee_pg_bind_text(st, "?3", phase);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      *out = strdup(v ? v : "");
      found = (*out != NULL) ? 1 : -1;
   }
   aimee_pg_finalize(st);
   return found;
}

/* Targeted update of the verdict columns only (NOT the pipeline state). A unit
 * row may not exist yet (snapshots can precede enumerate) — 0 rows is fine. */
static void cssr_record_verdict(void *conn, const char *project, const char *unit_path,
                                int oracle_equivalent, const char *note, const char *now_iso)
{
   static const char *sql = "UPDATE css_migration_units"
                            " SET oracle_equivalent = ?3, note = ?4, updated_at = ?5"
                            " WHERE project = ?1 AND unit_path = ?2"
                            " AND generation=(SELECT current_generation FROM projects"
                            "   WHERE name=?1 AND lifecycle_state='current')";
   char err[CSSR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", unit_path);
   aimee_pg_bind_int(st, "?3", oracle_equivalent);
   aimee_pg_bind_text(st, "?4", note ? note : "");
   aimee_pg_bind_text(st, "?5", now_iso ? now_iso : "");
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_css_render_oracle_evaluate(const char *project, const char *unit_path, const char *now_iso,
                                   css_render_verdict_t *out)
{
   if (!project || !unit_path || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!cssr_enabled())
   {
      snprintf(out->summary, sizeof(out->summary), "rendered oracle disabled");
      return 0;
   }
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char *bjson = NULL, *ajson = NULL;
   db2_css_render_snapshot_get(project, unit_path, "before", &bjson);
   db2_css_render_snapshot_get(project, unit_path, "after", &ajson);

   int before_valid = 0, after_valid = 0;
   if (db2_css_render_compare(bjson, ajson, &before_valid, &after_valid, &out->available,
                              &out->equivalent, &out->diff_count) != 0)
   {
      free(bjson);
      free(ajson);
      return -1;
   }

   int oracle_equivalent;
   if (!out->available)
   {
      oracle_equivalent = -1; /* unknown — conservative */
      snprintf(out->summary, sizeof(out->summary),
               "rendered oracle: unknown (%s%s snapshot missing)", before_valid ? "" : "before",
               after_valid ? "" : (before_valid ? "after" : "/after"));
   }
   else if (out->equivalent)
   {
      oracle_equivalent = 1;
      snprintf(out->summary, sizeof(out->summary), "rendered oracle: equivalent");
   }
   else
   {
      oracle_equivalent = 0;
      snprintf(out->summary, sizeof(out->summary), "rendered oracle: %d computed-style diff(s)",
               out->diff_count);
   }

   cssr_record_verdict(conn, project, unit_path, oracle_equivalent, out->summary, now_iso);

   free(bjson);
   free(ajson);
   return 0;
}
