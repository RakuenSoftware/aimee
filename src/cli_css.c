/* cli_css.c: `aimee css <op> <project>` — surfaces the CSS migration assistant's
 * server-side signals (style-graph derived signals, component-join dead rules /
 * unresolved classes, and the migration pipeline driver) over /v1/css/signals.
 * Requires `css_style_graph_enabled` and a re-index to populate the graph. */
#include "cli_css.h"

#include "cJSON.h"
#include "cli_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CSS_OPS[] = {"dead-rules",          "conflicts",  "duplicate-declarations",
                                      "duplicate-selectors", "unresolved", "migrate-enumerate",
                                      "migrate-list",        "rules-doc",  "assert-conventions",
                                      "conventions",         NULL};

static int css_op_known(const char *op)
{
   for (int i = 0; CSS_OPS[i]; i++)
      if (strcmp(CSS_OPS[i], op) == 0)
         return 1;
   return 0;
}

static void css_usage(void)
{
   fprintf(stderr, "usage: aimee css <op> <project> [--json]\n"
                   "  ops: dead-rules | conflicts | duplicate-declarations |\n"
                   "       duplicate-selectors | unresolved | migrate-enumerate |\n"
                   "       migrate-list | rules-doc | assert-conventions | conventions\n");
}

static void css_print_results(const char *op, cJSON *resp)
{
   if (strcmp(op, "rules-doc") == 0)
   {
      cJSON *doc = cJSON_GetObjectItemCaseSensitive(resp, "rules_doc");
      if (cJSON_IsString(doc))
         printf("%s\n", doc->valuestring);
      return;
   }
   if (strcmp(op, "migrate-enumerate") == 0)
   {
      cJSON *u = cJSON_GetObjectItemCaseSensitive(resp, "units");
      printf("enumerated %d migration unit(s)\n", cJSON_IsNumber(u) ? u->valueint : 0);
      return;
   }
   if (strcmp(op, "assert-conventions") == 0)
   {
      cJSON *a = cJSON_GetObjectItemCaseSensitive(resp, "asserted");
      printf("asserted %d convention fact(s)\n", cJSON_IsNumber(a) ? a->valueint : 0);
      return;
   }
   if (strcmp(op, "conventions") == 0)
   {
      cJSON *results = cJSON_GetObjectItemCaseSensitive(resp, "results");
      printf("conventions: %d fact(s)\n", cJSON_GetArraySize(results));
      cJSON *row = NULL;
      cJSON_ArrayForEach(row, results)
      {
         const cJSON *rel = cJSON_GetObjectItemCaseSensitive(row, "relation");
         const cJSON *val = cJSON_GetObjectItemCaseSensitive(row, "value");
         const cJSON *src = cJSON_GetObjectItemCaseSensitive(row, "source");
         printf("  %s = %s", cJSON_IsString(rel) ? rel->valuestring : "?",
                cJSON_IsString(val) ? val->valuestring : "?");
         if (cJSON_IsString(src))
            printf("  (%s)", src->valuestring);
         printf("\n");
      }
      return;
   }
   cJSON *results = cJSON_GetObjectItemCaseSensitive(resp, "results");
   int n = cJSON_GetArraySize(results);
   printf("%s: %d result(s)\n", op, n);
   cJSON *row = NULL;
   cJSON_ArrayForEach(row, results)
   {
      const cJSON *file = cJSON_GetObjectItemCaseSensitive(row, "file");
      const cJSON *sel = cJSON_GetObjectItemCaseSensitive(row, "selector");
      const cJSON *prop = cJSON_GetObjectItemCaseSensitive(row, "property");
      const cJSON *tok = cJSON_GetObjectItemCaseSensitive(row, "class_token");
      const cJSON *unit = cJSON_GetObjectItemCaseSensitive(row, "state");
      printf("  ");
      if (cJSON_IsString(file))
         printf("%s ", file->valuestring);
      if (cJSON_IsString(sel))
         printf("%s ", sel->valuestring);
      if (cJSON_IsString(prop))
         printf("{%s} ", prop->valuestring);
      if (cJSON_IsString(tok))
         printf(".%s ", tok->valuestring);
      if (cJSON_IsString(unit))
      {
         const cJSON *up = cJSON_GetObjectItemCaseSensitive(row, "unit");
         printf("%s [%s]", cJSON_IsString(up) ? up->valuestring : "", unit->valuestring);
      }
      printf("\n");
   }
}

int handle_css(int argc, char **argv, int json_output)
{
   if (argc < 2)
   {
      css_usage();
      return 2;
   }
   const char *op = argv[0];
   const char *project = argv[1];
   if (!css_op_known(op))
   {
      fprintf(stderr, "aimee css: unknown op '%s'\n", op);
      css_usage();
      return 2;
   }

   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
   {
      fprintf(stderr, "aimee css: no server configured (set `aimee remote`)\n");
      return 2;
   }
   char *bearer = cli_v1_client_bearer();
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "op", op);
   cJSON_AddStringToObject(body, "project", project);
   char *body_s = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   int status = 0;
   cJSON *resp =
       cli_http_request(endpoint, "POST", "/v1/css/signals", body_s, bearer, 60000, &status);
   free(endpoint);
   free(bearer);
   free(body_s);
   if (!resp || status != 200)
   {
      fprintf(stderr, "aimee css: server query failed (status %d)\n", status);
      cJSON_Delete(resp);
      return 1;
   }

   if (json_output)
   {
      char *s = cJSON_Print(resp);
      if (s)
      {
         printf("%s\n", s);
         free(s);
      }
   }
   else
   {
      css_print_results(op, resp);
   }
   cJSON_Delete(resp);
   return 0;
}
