/* kb_http_search.c: POST /v1/search typed-facet artifact filter (deep-curator).
 *
 * Split out of kb_http.c so the route dispatcher stays under the file-size
 * limit. The precision guarantee lives in db2_artifact_filter_facets; this file
 * only parses the `filters` object and shapes the response. Response JSON is
 * built with cJSON so escaping is handled for us. */

#include "kb_http_search.h"
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/kb_releases.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KBHS_MAX_HITS 100

/* Pick a short excerpt from a known narrative field of the payload. */
static void kbhs_excerpt(const char *payload_json, char *out, size_t out_cap)
{
   out[0] = '\0';
   cJSON *pl = payload_json ? cJSON_Parse(payload_json) : NULL;
   if (!pl)
      return;
   static const char *const keys[] = {"summary", "narrative", "intent", "title"};
   for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
   {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(pl, keys[k]);
      if (cJSON_IsString(v) && v->valuestring[0])
      {
         snprintf(out, out_cap, "%s", v->valuestring);
         break;
      }
   }
   cJSON_Delete(pl);
}

int kb_http_search_facets(const char *body, const char *project, int all_projects, char *out_buf,
                          int out_cap)
{
   cJSON *req = cJSON_Parse(body ? body : "");
   cJSON *filters = req ? cJSON_GetObjectItemCaseSensitive(req, "filters") : NULL;
   if (!cJSON_IsObject(filters) || cJSON_GetArraySize(filters) == 0)
   {
      cJSON_Delete(req);
      return -1; /* no filters → caller falls through to ranked search */
   }

   const cJSON *fs = cJSON_GetObjectItemCaseSensitive(filters, "status");
   const cJSON *fpr = cJSON_GetObjectItemCaseSensitive(filters, "priority");
   const cJSON *fk = cJSON_GetObjectItemCaseSensitive(filters, "kind");
   const cJSON *fc = cJSON_GetObjectItemCaseSensitive(filters, "component");

   const cJSON *mr = cJSON_GetObjectItemCaseSensitive(req, "max_results");
   int fmax = cJSON_IsNumber(mr) ? (int)mr->valuedouble : 10;
   if (fmax < 1)
      fmax = 1;
   if (fmax > KBHS_MAX_HITS)
      fmax = KBHS_MAX_HITS;

   /* Bind to the active release by default; an explicit "release_id" in the
    * body overrides (0 means search across all releases). */
   const cJSON *rel = cJSON_GetObjectItemCaseSensitive(req, "release_id");
   int64_t release_id =
       cJSON_IsNumber(rel) ? (int64_t)rel->valuedouble : db2_kb_release_get_active();

   db2_artifact_row_t *rows = calloc((size_t)fmax, sizeof(*rows));
   int rn = -1;
   if (rows)
   {
      const char *kind = cJSON_IsString(fk) ? fk->valuestring : NULL;
      const char *status = cJSON_IsString(fs) ? fs->valuestring : NULL;
      const char *priority = cJSON_IsString(fpr) ? fpr->valuestring : NULL;
      const char *component = cJSON_IsString(fc) ? fc->valuestring : NULL;
      if (all_projects && project && project[0])
      {
         rn = db2_artifact_filter_facets_scoped(release_id, project, NULL, kind, status, priority,
                                                component, rows, fmax);
         if (rn >= 0 && rn < fmax)
         {
            int tail = db2_artifact_filter_facets_scoped(release_id, NULL, project, kind, status,
                                                         priority, component, rows + rn, fmax - rn);
            rn = tail < 0 ? -1 : rn + tail;
         }
      }
      else
         rn = db2_artifact_filter_facets_scoped(release_id, all_projects ? NULL : project, NULL,
                                                kind, status, priority, component, rows, fmax);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON *hits = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; i < rn; i++)
   {
      char excerpt[256];
      kbhs_excerpt(rows[i].payload_json, excerpt, sizeof(excerpt));

      cJSON *hit = cJSON_CreateObject();
      cJSON_AddStringToObject(hit, "project",
                              strcmp(rows[i].scope_kind, "project") == 0 ? rows[i].scope_id : "");
      cJSON_AddStringToObject(hit, "artifact_id", rows[i].id);
      cJSON_AddNumberToObject(hit, "score", 1.0);
      cJSON_AddStringToObject(hit, "kind", rows[i].kind);
      cJSON_AddStringToObject(hit, "scope_kind", rows[i].scope_kind);
      cJSON_AddStringToObject(hit, "scope_id", rows[i].scope_id);
      cJSON_AddStringToObject(hit, "excerpt", excerpt);
      cJSON_AddItemToObject(hit, "citations", cJSON_CreateArray());
      cJSON_AddItemToArray(hits, hit);
   }
   cJSON_AddNullToObject(resp, "next_cursor");
   cJSON_AddNumberToObject(resp, "total_hits", rn < 0 ? 0 : rn);
   cJSON_AddStringToObject(resp, "fusion_mode_used", "facet");
   /* Cite the release the results are bound to (null when unbound). */
   if (release_id > 0)
      cJSON_AddNumberToObject(resp, "release_id", (double)release_id);
   else
      cJSON_AddNullToObject(resp, "release_id");

   char *json = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   free(rows);
   cJSON_Delete(req);

   int status = 200;
   if (rn < 0 || !json)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"facet search unavailable\"}");
      status = 503;
   }
   else if ((int)strlen(json) + 1 > out_cap)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"response too large\"}");
      status = 500;
   }
   else
   {
      snprintf(out_buf, (size_t)out_cap, "%s", json);
   }
   free(json);
   return status;
}
