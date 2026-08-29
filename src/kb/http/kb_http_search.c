/* kb_http_search.c: POST /v1/search typed-facet artifact filter (deep-curator).
 *
 * Split out of kb_http.c so the route dispatcher stays under the file-size
 * limit. The precision guarantee lives in db2_artifact_filter_facets; this file
 * only parses the `filters` object and shapes the response. Response JSON is
 * built with cJSON so escaping is handled for us. */

#include "kb_http_search.h"
#include "kb_reqctx.h"
#include "kb_scope.h"
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/kb_releases.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KBHS_MAX_HITS 100

static int kbhs_error(char *out_buf, int out_cap, int status, const char *message)
{
   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"%s\"}", message);
   return status;
}

int kb_http_search_project_scope(const char *body, char *project, size_t project_cap,
                                 int *all_projects, char *out_buf, int out_cap)
{
   project[0] = '\0';
   *all_projects = 0;
   cJSON *root = cJSON_Parse(body ? body : "{}");
   if (!root)
      return kbhs_error(out_buf, out_cap, 400, "invalid json");
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "project");
   const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "scope");
   /* Settle the shape of "scope" before reading it: absent defaults to
    * "current", present-but-not-a-string is a client error. Reading
    * js->valuestring first and only then testing `js` for NULL read as a
    * null-deref to cppcheck (nullPointerRedundantCheck), which is what the
    * ratchet caught -- and it is genuinely harder to follow. After this guard
    * js is either NULL or a string, so the default below needs no type test. */
   if (js && !cJSON_IsString(js))
   {
      cJSON_Delete(root);
      return kbhs_error(out_buf, out_cap, 400, "scope must be current or all");
   }
   const char *scope = js ? js->valuestring : "current";
   if (strcmp(scope, "current") != 0 && strcmp(scope, "all") != 0)
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":{\"type\":\"invalid_scope\",\"message\":\"scope must be current or "
               "all\"}}");
      return 400;
   }
   if (cJSON_IsString(jp) && jp->valuestring[0])
   {
      if (strlen(jp->valuestring) >= project_cap)
      {
         cJSON_Delete(root);
         return kbhs_error(out_buf, out_cap, 400, "project id is too long");
      }
      snprintf(project, project_cap, "%s", jp->valuestring);
   }

   const char *verified_kind = NULL;
   const char *verified_id = NULL;
   int verified = kb_reqctx_verified_scope(&verified_kind, &verified_id);
   if (strcmp(scope, "all") == 0)
   {
      cJSON_Delete(root);
      /* A service credential is the managed deployment's data-plane identity:
       * it spans projects but remains tenant-bounded by the resolved caller
       * context and is still excluded from administrative routes. */
      if (verified && strcmp(verified_kind, KB_SCOPE_KIND_SERVICE) != 0)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":{\"type\":\"forbidden\",\"message\":\"a scoped credential "
                  "cannot search all projects\"}}");
         return 403;
      }
      *all_projects = 1;
      return 0;
   }

   if (!project[0] && verified && strcmp(verified_kind, "project") == 0)
      snprintf(project, project_cap, "%s", verified_id);
   if (project[0] && verified && strcmp(verified_kind, "project") == 0 &&
       strcmp(project, verified_id) != 0)
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":{\"type\":\"forbidden\",\"message\":\"project is outside the "
               "verified credential scope\"}}");
      return 403;
   }
   cJSON_Delete(root);
   if (project[0])
      return 0;
   snprintf(out_buf, (size_t)out_cap,
            "{\"error\":{\"type\":\"scope_required\",\"message\":\"no active project is "
            "available; pass project or scope=all explicitly\"}}");
   return 409;
}

int kb_http_search_validate_backend(const char *body, char *out_buf, int out_cap)
{
   cJSON *root = cJSON_Parse(body);
   const cJSON *error = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
   const cJSON *results = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : NULL;
   int status = 0;
   if (cJSON_IsString(error))
   {
      snprintf(out_buf, (size_t)out_cap, "%s", body);
      status = 503;
   }
   else if (!cJSON_IsObject(root) || !cJSON_IsArray(results))
      status = kbhs_error(out_buf, out_cap, 503, "invalid search backend response");
   cJSON_Delete(root);
   return status;
}

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
