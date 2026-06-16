/* kb_service_css.c: KB-side handler for the CSS migration assistant signals
 * (css.signals). The style graph + component join + migration tables live in the
 * KB's DB2 (the KB runs the indexer), so the queries run here; aimee-server
 * forwards the op via kb_client. Mirrors the other kb_service_* handlers. */
#include "aimee.h"
#include "cJSON.h"
#include "db2/css_graph.h"
#include "db2/css_migration.h"

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

   cJSON *results = css_signal_array(op, project);
   cJSON_AddItemToObject(resp, "results", results);
   cJSON_AddNumberToObject(resp, "count", cJSON_GetArraySize(results));
   return kb_send_response(fd, resp);
}
