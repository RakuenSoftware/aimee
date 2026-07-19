/*
 * roadmap_report.c: HTML progress report generation for spec-driven roadmaps.
 */

#include "aimee.h"
#include "roadmap_report.h"
#include "roadmap.h"
#include "headers/platform_path.h"
#include "db2/artifacts.h"
#include "db2/db_postgres.h"
#include "db2/db2_internal.h"
#include "headers/dstr.h"
#include "db1/roadmap_runtime.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── path helper ────────────────────────────────────────────────────────────── */

char *roadmap_report_path(const char *roadmap_id, char *buf, size_t len)
{
   if (!roadmap_id || !roadmap_id[0])
      return NULL;

   snprintf(buf, len, ".aimee/roadmap/reports/%s.html", roadmap_id);
   return buf;
}

/* ── string helpers ─────────────────────────────────────────────────────────── */

static const char *obj_str(const cJSON *obj, const char *key, const char *dflt)
{
   const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!cJSON_IsString(j))
      return dflt;
   return j->valuestring;
}

/* ── report writer ──────────────────────────────────────────────────────────── */

int roadmap_report_write(const char *roadmap_id)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;

   /* 1. Build the path. */
   char path_buf[512];
   if (!roadmap_report_path(roadmap_id, path_buf, sizeof(path_buf)))
      return -1;

   /* 2. Create the directory tree. */
   if (platform_mkdir_p(".aimee/roadmap/reports", 0755) != 0)
   {
      /* Fallback: create components explicitly. */
      mkdir(".aimee", 0755);
      mkdir(".aimee/roadmap", 0755);
      mkdir(".aimee/roadmap/reports", 0755);
   }

   /* 3. Load the roadmap artifact. */
   db2_artifact_row_t row;
   memset(&row, 0, sizeof(row));
   if (db2_artifact_read(roadmap_id, &row, NULL, 0, NULL) != 0)
      return -1;

   cJSON *rp = cJSON_Parse(row.payload_json);
   if (!rp)
      return -1;

   const char *goal = obj_str(rp, "goal", "(unknown)");
   const char *rm_status = "";
   const char *rm_phase = "";
   rdm_dispatch_t disp;
   memset(&disp, 0, sizeof(disp));
   if (rdm_dispatch_get(roadmap_id, &disp) == 0)
   {
      rm_status = disp.status;
      rm_phase = disp.phase;
   }

   /* 4. Load all plan_unit artifacts scoped to roadmap_id. */
   void *conn = db2_conn();
   if (!conn)
   {
      cJSON_Delete(rp);
      return -1;
   }

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, payload FROM artifacts "
                                          "WHERE kind = ?1 AND scope_id = ?2 AND state = ?3",
                                          err, sizeof(err));
   if (!st)
   {
      cJSON_Delete(rp);
      return -1;
   }

   aimee_pg_bind_text(st, "?1", "plan_unit");
   aimee_pg_bind_text(st, "?2", roadmap_id);
   aimee_pg_bind_text(st, "?3", "committed");

/* Pre-collect unit info to sort by id. */
#define MAX_UNITS 256
   char unit_ids[MAX_UNITS][37];
   char unit_payloads[MAX_UNITS][4096];
   int unit_count = 0;

   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (unit_count >= MAX_UNITS)
         break;

      const char *uid = aimee_pg_column_text(st, 0);
      const char *pl = aimee_pg_column_text(st, 1);
      if (!uid || !pl)
         continue;

      strncpy(unit_ids[unit_count], uid, 36);
      unit_ids[unit_count][36] = '\0';
      strncpy(unit_payloads[unit_count], pl, 4095);
      unit_payloads[unit_count][4095] = '\0';
      unit_count++;
   }

   aimee_pg_finalize(st);

   /* 5. Build HTML. */
   dstr_t html;
   dstr_init(&html);

   dstr_append_str(&html, "<!DOCTYPE html>\n"
                          "<html><head><meta charset=\"utf-8\">"
                          "<title>Roadmap: ");
   dstr_append_str(&html, goal);
   dstr_append_str(&html,
                   "</title><style>\n"
                   "body{font-family:sans-serif;max-width:900px;margin:2em auto;padding:0 1em}\n"
                   "h1{border-bottom:2px solid #333}\n"
                   ".unit{margin:.5em 0;padding:.5em;border-left:4px solid #ccc}\n"
                   ".done{border-color:#2d8a4e;background:#f0fff4}\n"
                   ".active{border-color:#1a73e8;background:#e8f0fe}\n"
                   ".needs_review{border-color:#d93025;background:#fce8e6}\n"
                   ".pending{border-color:#aaa}\n"
                   ".blocked{border-color:#f90;background:#fffde7}\n"
                   "</style></head><body>\n");

   dstr_append_str(&html, "<h1>Roadmap: ");
   dstr_append_str(&html, goal);
   dstr_append_str(&html, "</h1>\n");

   dstr_append_str(&html, "<p>Status: ");
   dstr_append_str(&html, rm_status);
   dstr_append_str(&html, " | Phase: ");
   dstr_append_str(&html, rm_phase);
   dstr_append_str(&html, "</p>\n");

   dstr_append_str(&html, "<h2>Units</h2>\n");

   for (int i = 0; i < unit_count; i++)
   {
      cJSON *up = cJSON_Parse(unit_payloads[i]);
      if (!up)
         continue;

      const char *level = obj_str(up, "level", "?");
      const char *title = obj_str(up, "title", "(untitled)");
      const char *state = obj_str(up, "state", "pending");
      const char *st_cls = state;
      if (strcmp(state, "done") != 0 && strcmp(state, "pending") != 0 &&
          strcmp(state, "active") != 0 && strcmp(state, "blocked") != 0 &&
          strcmp(state, "needs_review") != 0)
         st_cls = "pending";

      dstr_append_str(&html, "<div class=\"unit ");
      dstr_append_str(&html, st_cls);
      dstr_append_str(&html, "\"><b>");
      dstr_append_str(&html, level);
      dstr_append_str(&html, "</b>: ");
      dstr_append_str(&html, title);
      dstr_append_str(&html, " (<em>");
      dstr_append_str(&html, state);
      dstr_append_str(&html, "</em>)</div>\n");

      cJSON_Delete(up);
   }

   dstr_append_str(&html, "</body></html>\n");

   cJSON_Delete(rp);

   /* 6. Write to file. */
   FILE *fp = fopen(path_buf, "w");
   if (!fp)
   {
      dstr_free(&html);
      return -1;
   }

   int rc = 0;
   if (fwrite(html.data, 1, html.len, fp) != html.len)
      rc = -1;

   fclose(fp);
   dstr_free(&html);

   return rc;
}
