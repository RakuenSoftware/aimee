/* kb_service_css.c: KB-side handler for the CSS migration assistant signals
 * (css.signals). The style graph + component join + migration tables live in the
 * KB's DB2 (the KB runs the indexer), so the queries run here; aimee-server
 * forwards the op via kb_client. Mirrors the other kb_service_* handlers. */
#include "aimee.h"
#include "cJSON.h"
#include "css_render_oracle.h"
#include "db2/css_graph.h"
#include "db2/css_migration.h"
#include "db2/css_render.h"
#include "db2/typed_facts.h"

#include <string.h>

int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);

static cJSON *css_signal_array(const char *op, const char *project)
{
   cJSON *arr = cJSON_CreateArray();
   if (strcmp(op, "dead-rules") == 0)
   {
      css_dead_rule_hit_t h[512];
      int n = db2_css_dead_rules(project, h, 512);
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "file", h[i].file_path);
         cJSON_AddStringToObject(o, "selector", h[i].selector);
         cJSON_AddNumberToObject(o, "line", h[i].line);
         cJSON_AddItemToArray(arr, o);
      }
   }
   else if (strcmp(op, "conflicts") == 0)
   {
      css_spec_conflict_t h[512];
      int n = db2_css_graph_specificity_conflicts(project, h, 512);
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "file", h[i].file_path);
         cJSON_AddStringToObject(o, "property", h[i].property);
         cJSON_AddStringToObject(o, "winner_selector", h[i].winner_selector);
         cJSON_AddNumberToObject(o, "winner_line", h[i].winner_line);
         cJSON_AddStringToObject(o, "loser_selector", h[i].loser_selector);
         cJSON_AddNumberToObject(o, "loser_line", h[i].loser_line);
         cJSON_AddItemToArray(arr, o);
      }
   }
   else if (strcmp(op, "duplicate-declarations") == 0)
   {
      css_dup_decl_t h[512];
      int n = db2_css_graph_duplicate_declarations(project, h, 512);
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "file", h[i].file_path);
         cJSON_AddStringToObject(o, "property", h[i].property);
         cJSON_AddStringToObject(o, "value", h[i].value);
         cJSON_AddNumberToObject(o, "count", h[i].count);
         cJSON_AddItemToArray(arr, o);
      }
   }
   else if (strcmp(op, "duplicate-selectors") == 0)
   {
      css_dup_selector_t h[512];
      int n = db2_css_graph_duplicate_selectors(project, h, 512);
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "file", h[i].file_path);
         cJSON_AddStringToObject(o, "selector", h[i].selector);
         cJSON_AddNumberToObject(o, "count", h[i].count);
         cJSON_AddItemToArray(arr, o);
      }
   }
   else if (strcmp(op, "unresolved") == 0)
   {
      css_unresolved_hit_t h[512];
      int n = db2_css_component_unresolved(project, h, 512);
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "file", h[i].file_path);
         cJSON_AddStringToObject(o, "class_token", h[i].class_token);
         cJSON_AddItemToArray(arr, o);
      }
   }
   else if (strcmp(op, "migrate-list") == 0)
   {
      css_migration_unit_t u[1024];
      int n = db2_css_migration_list(project, NULL, u, 1024);
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "unit", u[i].unit_path);
         cJSON_AddStringToObject(o, "state", u[i].state);
         cJSON_AddNumberToObject(o, "total_tokens", u[i].total_tokens);
         cJSON_AddNumberToObject(o, "resolved_tokens", u[i].resolved_tokens);
         cJSON_AddNumberToObject(o, "oracle_equivalent", u[i].oracle_equivalent);
         cJSON_AddItemToArray(arr, o);
      }
   }
   return arr;
}

int kb_handle_css_signals(int fd, cJSON *req)
{
   cJSON *op_j = cJSON_GetObjectItemCaseSensitive(req, "op");
   cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(req, "project");
   if (!cJSON_IsString(op_j) || !op_j->valuestring[0])
      return kb_send_error(fd, "css.signals requires 'op'");
   if (!cJSON_IsString(proj_j) || !proj_j->valuestring[0])
      return kb_send_error(fd, "css.signals requires 'project'");
   const char *op = op_j->valuestring;
   const char *project = proj_j->valuestring;

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "op", op);
   cJSON_AddStringToObject(resp, "project", project);

   if (strcmp(op, "migrate-enumerate") == 0)
   {
      int n = db2_css_migration_enumerate(project);
      if (n < 0)
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css migrate-enumerate failed");
      }
      cJSON_AddNumberToObject(resp, "units", n);
      return kb_send_response(fd, resp);
   }
   if (strcmp(op, "rules-doc") == 0)
   {
      char doc[16384];
      int n = db2_css_migration_rules_doc(project, doc, sizeof(doc));
      if (n < 0)
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css rules-doc failed");
      }
      cJSON_AddStringToObject(resp, "rules_doc", doc);
      return kb_send_response(fd, resp);
   }
   if (strcmp(op, "assert-conventions") == 0)
   {
      /* #2-upgrade: promote the exemplar's machine-derivable conventions into
       * typed facts. No-op (returns 0) unless both css_style_graph_enabled and
       * typed_facts_enabled are set. */
      char now_iso[40];
      now_utc(now_iso, sizeof(now_iso));
      int n = db2_css_migration_assert_conventions(project, now_iso);
      if (n < 0)
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css assert-conventions failed");
      }
      cJSON_AddNumberToObject(resp, "asserted", n);
      return kb_send_response(fd, resp);
   }
   if (strcmp(op, "render-store") == 0)
   {
      /* #4-full evidence ingest: a (sandboxed, out-of-process) render backend's
       * computed-style snapshot JSON for one unit/phase enters here. */
      cJSON *unit_j = cJSON_GetObjectItemCaseSensitive(req, "unit");
      cJSON *phase_j = cJSON_GetObjectItemCaseSensitive(req, "phase");
      cJSON *snap_j = cJSON_GetObjectItemCaseSensitive(req, "snapshot");
      if (!cJSON_IsString(unit_j) || !cJSON_IsString(phase_j) || !cJSON_IsString(snap_j))
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css render-store requires 'unit', 'phase', 'snapshot'");
      }
      char now_iso[40];
      now_utc(now_iso, sizeof(now_iso));
      int n = db2_css_render_snapshot_store(project, unit_j->valuestring, phase_j->valuestring,
                                            snap_j->valuestring, now_iso);
      if (n < 0)
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css render-store failed (invalid phase or db error)");
      }
      cJSON_AddStringToObject(resp, "unit", unit_j->valuestring);
      cJSON_AddStringToObject(resp, "phase", phase_j->valuestring);
      cJSON_AddNumberToObject(resp, "stored", n); /* 1 stored, 0 gated-off */
      return kb_send_response(fd, resp);
   }
   if (strcmp(op, "render-capture") == 0)
   {
      /* #4-full slice 3: render a unit/phase's html+css via the configured render
       * backend and store the resulting computed-style snapshot. Closes the loop
       * render -> store -> (render-verify). */
      cJSON *unit_j = cJSON_GetObjectItemCaseSensitive(req, "unit");
      cJSON *phase_j = cJSON_GetObjectItemCaseSensitive(req, "phase");
      cJSON *html_j = cJSON_GetObjectItemCaseSensitive(req, "html");
      cJSON *css_j = cJSON_GetObjectItemCaseSensitive(req, "css");
      if (!cJSON_IsString(unit_j) || !cJSON_IsString(phase_j) || !cJSON_IsString(html_j) ||
          !cJSON_IsString(css_j))
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css render-capture requires 'unit', 'phase', 'html', 'css'");
      }
      char *snap = NULL, *rerr = NULL;
      css_render_status_t st =
          css_render_oracle_render(html_j->valuestring, css_j->valuestring, &snap, &rerr);
      if (st == CSS_RENDER_UNAVAILABLE)
      {
         cJSON_Delete(resp);
         free(snap);
         free(rerr);
         return kb_send_error(fd, "no render backend configured (set css_render_command)");
      }
      if (st != CSS_RENDER_OK || !snap)
      {
         char msg[160];
         snprintf(msg, sizeof(msg), "render failed: %s", rerr ? rerr : "unknown");
         cJSON_Delete(resp);
         free(snap);
         free(rerr);
         return kb_send_error(fd, msg);
      }
      free(rerr);
      char now_iso[40];
      now_utc(now_iso, sizeof(now_iso));
      int n = db2_css_render_snapshot_store(project, unit_j->valuestring, phase_j->valuestring,
                                            snap, now_iso);
      free(snap);
      if (n < 0)
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css render-capture: store failed (invalid phase or db error)");
      }
      cJSON_AddStringToObject(resp, "unit", unit_j->valuestring);
      cJSON_AddStringToObject(resp, "phase", phase_j->valuestring);
      cJSON_AddNumberToObject(resp, "stored", n);
      return kb_send_response(fd, resp);
   }
   if (strcmp(op, "render-verify") == 0)
   {
      cJSON *unit_j = cJSON_GetObjectItemCaseSensitive(req, "unit");
      if (!cJSON_IsString(unit_j))
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css render-verify requires 'unit'");
      }
      char now_iso[40];
      now_utc(now_iso, sizeof(now_iso));
      css_render_verdict_t v;
      if (db2_css_render_oracle_evaluate(project, unit_j->valuestring, now_iso, &v) != 0)
      {
         cJSON_Delete(resp);
         return kb_send_error(fd, "css render-verify failed");
      }
      cJSON_AddStringToObject(resp, "unit", unit_j->valuestring);
      cJSON_AddBoolToObject(resp, "available", v.available);
      cJSON_AddBoolToObject(resp, "equivalent", v.equivalent);
      cJSON_AddNumberToObject(resp, "diff_count", v.diff_count);
      cJSON_AddStringToObject(resp, "summary", v.summary);
      cJSON_AddStringToObject(resp, "limitation", css_render_oracle_limitation_banner());
      return kb_send_response(fd, resp);
   }
   if (strcmp(op, "conventions") == 0)
   {
      /* Recall the project's active convention facts (naming_convention,
       * token_strategy, ...) — every typed fact whose subject is the project. */
      cJSON *arr = cJSON_CreateArray();
      typed_fact_t tf[64];
      int n = db2_typed_fact_recall(project, NULL, tf, 64);
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddStringToObject(o, "relation", tf[i].relation);
         cJSON_AddStringToObject(o, "value", tf[i].object);
         cJSON_AddNumberToObject(o, "confidence", tf[i].confidence);
         cJSON_AddStringToObject(o, "source", tf[i].source);
         cJSON_AddStringToObject(o, "asserted_at", tf[i].asserted_at);
         cJSON_AddItemToArray(arr, o);
      }
      cJSON_AddItemToObject(resp, "results", arr);
      cJSON_AddNumberToObject(resp, "count", n);
      return kb_send_response(fd, resp);
   }

   cJSON *results = css_signal_array(op, project);
   cJSON_AddItemToObject(resp, "results", results);
   cJSON_AddNumberToObject(resp, "count", cJSON_GetArraySize(results));
   return kb_send_response(fd, resp);
}
