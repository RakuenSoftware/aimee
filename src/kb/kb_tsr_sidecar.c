/* kb_tsr_sidecar.c: TSR sidecar client. See kb_tsr_sidecar.h.
 *
 * Transport is the in-process agent_http_post (the same one the embedder uses; linked by
 * aimee-kb). No fork, no document-content retention. A missing/unreachable sidecar or a
 * malformed response is reported as -1 (unavailable) so the caller degrades to text-only
 * rather than failing the ingest. */
#include "kb_tsr_sidecar.h"

#include "agent_exec.h" /* agent_http_post */
#include "cJSON.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_TSR_TIMEOUT_MS 30000
#define KB_TSR_MAX_CELLS  4096

const char *kb_tsr_endpoint(const config_t *cfg)
{
   if (cfg && cfg->tsr_command[0])
      return cfg->tsr_command;
   const char *env = getenv("AIMEE_TSR_URL");
   if (env && env[0])
      return env;
   return "";
}

void kb_tsr_free_cells(kb_tsr_cell_t *cells, int n)
{
   (void)n;
   free(cells);
}

static void copy_field(char *dst, size_t cap, cJSON *obj, const char *key)
{
   cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (cJSON_IsString(it) && it->valuestring)
      snprintf(dst, cap, "%s", it->valuestring);
}

static int json_int(cJSON *obj, const char *key, int dflt)
{
   cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(it) ? (int)it->valuedouble : dflt;
}

int kb_tsr_recognize(const char *endpoint, int page_no, const char *page_json,
                     kb_tsr_cell_t **cells_out, int *n_out)
{
   if (cells_out)
      *cells_out = NULL;
   if (n_out)
      *n_out = 0;
   if (!endpoint || !endpoint[0] || !page_json)
      return -1;

   /* Request: {"page_no":N,"regions":<page_json array>} */
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddNumberToObject(req, "page_no", page_no);
   cJSON *regions = cJSON_Parse(page_json);
   cJSON_AddItemToObject(req, "regions", regions ? regions : cJSON_CreateArray());
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   char *resp = NULL;
   int status = agent_http_post(endpoint, NULL, body, &resp, KB_TSR_TIMEOUT_MS, NULL);
   free(body);
   if (status < 200 || status >= 300 || !resp)
   {
      free(resp);
      LOG_WARN("kb_tsr", "TSR sidecar call failed (status=%d)", status);
      return -1;
   }

   cJSON *root = cJSON_Parse(resp);
   free(resp);
   if (!root)
   {
      LOG_WARN("kb_tsr", "TSR sidecar returned unparseable JSON");
      return -1;
   }

   cJSON *is_table = cJSON_GetObjectItemCaseSensitive(root, "is_table");
   cJSON *cells = cJSON_GetObjectItemCaseSensitive(root, "cells");
   if (cJSON_IsBool(is_table) && !cJSON_IsTrue(is_table))
   {
      cJSON_Delete(root);
      return 0; /* sidecar ran, found no table */
   }
   if (!cJSON_IsArray(cells) || cJSON_GetArraySize(cells) == 0)
   {
      cJSON_Delete(root);
      return 0;
   }

   int count = cJSON_GetArraySize(cells);
   if (count > KB_TSR_MAX_CELLS)
      count = KB_TSR_MAX_CELLS;
   kb_tsr_cell_t *arr = calloc((size_t)count, sizeof(*arr));
   if (!arr)
   {
      cJSON_Delete(root);
      return -1;
   }

   int n = 0;
   cJSON *c = NULL;
   cJSON_ArrayForEach(c, cells)
   {
      if (n >= count)
         break;
      arr[n].row = json_int(c, "row", 0);
      arr[n].col = json_int(c, "col", 0);
      arr[n].confidence = json_int(c, "confidence", 0);
      arr[n].line_index = json_int(c, "line_index", -1);
      copy_field(arr[n].text, sizeof(arr[n].text), c, "text");
      copy_field(arr[n].subject, sizeof(arr[n].subject), c, "subject");
      copy_field(arr[n].relation, sizeof(arr[n].relation), c, "relation");
      copy_field(arr[n].object, sizeof(arr[n].object), c, "object");
      n++;
   }
   cJSON_Delete(root);
   *cells_out = arr;
   *n_out = n;
   return n > 0 ? 1 : 0;
}
