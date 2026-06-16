/* db2/css_render.c: storage + evaluation for the rendered computed-style oracle.
 * See css_render.h. */
#include "css_render.h"

#include "config.h" /* css_style_graph_enabled gate */
#include "css_render_oracle.h"
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSSR_ERRBUF 256

static int cssr_enabled(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 0;
   return cfg.css_style_graph_enabled ? 1 : 0;
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
                            " WHERE project = ?1 AND unit_path = ?2 AND phase = ?3";
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
                            " (project, unit_path, phase, snapshot, content_hash, captured_at)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", unit_path);
   aimee_pg_bind_text(st, "?3", phase);
   aimee_pg_bind_text(st, "?4", snapshot_json);
   aimee_pg_bind_text(st, "?5", hash);
   aimee_pg_bind_text(st, "?6", now_iso ? now_iso : "");
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 1 : -1;
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
   static const char *sql = "SELECT snapshot FROM css_render_snapshots"
                            " WHERE project = ?1 AND unit_path = ?2 AND phase = ?3";
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
                            " WHERE project = ?1 AND unit_path = ?2";
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

   css_render_snapshot_t *bs = bjson ? css_render_snapshot_parse(bjson) : NULL;
   css_render_snapshot_t *as = ajson ? css_render_snapshot_parse(ajson) : NULL;

   css_render_result_t *r = css_render_oracle_compare(bs, as);
   if (!r)
   {
      free(bjson);
      free(ajson);
      css_render_snapshot_free(bs);
      css_render_snapshot_free(as);
      return -1;
   }

   out->available = r->available;
   out->equivalent = r->equivalent;
   out->diff_count = r->diff_count;
   int oracle_equivalent;
   if (!r->available)
   {
      oracle_equivalent = -1; /* unknown — conservative */
      snprintf(out->summary, sizeof(out->summary),
               "rendered oracle: unknown (%s%s snapshot missing)", bs ? "" : "before",
               as ? "" : (bs ? "after" : "/after"));
   }
   else if (r->equivalent)
   {
      oracle_equivalent = 1;
      snprintf(out->summary, sizeof(out->summary), "rendered oracle: equivalent");
   }
   else
   {
      oracle_equivalent = 0;
      snprintf(out->summary, sizeof(out->summary), "rendered oracle: %d computed-style diff(s)",
               r->diff_count);
   }

   cssr_record_verdict(conn, project, unit_path, oracle_equivalent, out->summary, now_iso);

   css_render_result_free(r);
   css_render_snapshot_free(bs);
   css_render_snapshot_free(as);
   free(bjson);
   free(ajson);
   return 0;
}
