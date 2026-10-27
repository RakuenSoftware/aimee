/* kb_tsr_sidecar.c: TSR sidecar client. See kb_tsr_sidecar.h.
 *
 * Transport is the in-process byte-counted HTTP client (linked by
 * aimee-kb). No fork, no document-content retention. A missing/unreachable sidecar or a
 * malformed response is reported as -1 (unavailable) so the caller degrades to text-only
 * rather than failing the ingest. */
#include "kb_tsr_sidecar.h"

#include "agent_exec.h" /* agent_http_post */
#include "cJSON.h"
#include <aimee/ir/aimee_ir.h>
#include <limits.h>
#include <math.h>
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_TSR_TIMEOUT_MS 30000
#define KB_TSR_MAX_CELLS  4096
/* Cap the sidecar response before parsing so a misconfigured/hostile sidecar inside the KB
 * perimeter cannot OOM the ingest with an unbounded JSON body. */
#define KB_TSR_MAX_RESP_BYTES (8 * 1024 * 1024)

void kb_tsr_free_cells(kb_tsr_cell_t *cells, int n)
{
   (void)n;
   free(cells);
}

static int copy_field(char *dst, size_t capacity, const cJSON *row, const char *key)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);
   if (!value)
      return 0;
   if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity)
      return -1;
   memcpy(dst, value->valuestring, strlen(value->valuestring) + 1);
   return 0;
}

static int integer(const cJSON *row, const char *key, int fallback, int minimum, int maximum,
                   int *out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);
   if (!value)
   {
      *out = fallback;
      return 0;
   }
   if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < minimum ||
       value->valuedouble > maximum || floor(value->valuedouble) != value->valuedouble)
      return -1;
   *out = (int)value->valuedouble;
   return 0;
}

int kb_tsr_recognize(const char *endpoint, int page_no, const char *page_json,
                     kb_tsr_cell_t **cells_out, int *n_out)
{
   if (cells_out)
      *cells_out = NULL;
   if (n_out)
      *n_out = 0;
   if (!cells_out || !n_out || !endpoint || !endpoint[0] || !page_json)
      return -1;

   /* The page-region array is the documented domain request, not vendor JSON.
    * Invalid input must not be converted to a successful empty request. */
   cJSON *regions = aimee_ir_json_parse_frame(page_json, strlen(page_json));
   if (!cJSON_IsArray(regions))
   {
      cJSON_Delete(regions);
      return -1;
   }
   cJSON *req = cJSON_CreateObject();
   if (!req || !cJSON_AddNumberToObject(req, "page_no", page_no) ||
       !cJSON_AddItemToObject(req, "regions", regions))
   {
      cJSON_Delete(regions);
      cJSON_Delete(req);
      return -1;
   }
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   char *resp = NULL;
   size_t response_length = 0;
   int status =
       agent_http_post_content_type_bytes(endpoint, NULL, "application/json", body, strlen(body),
                                          &resp, &response_length, KB_TSR_TIMEOUT_MS, NULL);
   free(body);
   if (status < 200 || status >= 300 || !resp)
   {
      free(resp);
      LOG_WARN("kb_tsr", "TSR sidecar call failed (status=%d)", status);
      return -1;
   }
   if (response_length > KB_TSR_MAX_RESP_BYTES)
   {
      LOG_WARN("kb_tsr", "TSR sidecar response too large (%zu bytes); rejecting", response_length);
      free(resp);
      return -1;
   }

   cJSON *root = aimee_ir_json_parse_frame(resp, response_length);
   free(resp);
   if (!cJSON_IsObject(root))
   {
      LOG_WARN("kb_tsr", "TSR sidecar returned unparseable JSON");
      cJSON_Delete(root);
      return -1;
   }

   const cJSON *is_table = cJSON_GetObjectItemCaseSensitive(root, "is_table");
   const cJSON *cells = cJSON_GetObjectItemCaseSensitive(root, "cells");
   if ((is_table && !cJSON_IsBool(is_table)) || (cells && !cJSON_IsArray(cells)))
      goto invalid;
   if (cJSON_IsFalse(is_table))
   {
      if (cells && cJSON_GetArraySize(cells))
         goto invalid;
      cJSON_Delete(root);
      return 0;
   }
   if (!cells)
      goto invalid;
   int count = cJSON_GetArraySize(cells);
   if (!count)
   {
      cJSON_Delete(root);
      return 0;
   }
   if (count > KB_TSR_MAX_CELLS)
      goto invalid;
   kb_tsr_cell_t *arr = calloc((size_t)count, sizeof(*arr));
   if (!arr)
      goto invalid;
   int n = 0;
   const cJSON *cell;
   cJSON_ArrayForEach(cell, cells)
   {
      kb_tsr_cell_t value = {0};
      if (!cJSON_IsObject(cell) || integer(cell, "row", 0, 0, INT_MAX, &value.row) ||
          integer(cell, "col", 0, 0, INT_MAX, &value.col) ||
          integer(cell, "confidence", 0, 0, 100, &value.confidence) ||
          integer(cell, "line_index", -1, -1, INT_MAX, &value.line_index) ||
          copy_field(value.text, sizeof value.text, cell, "text") ||
          copy_field(value.subject, sizeof value.subject, cell, "subject") ||
          copy_field(value.relation, sizeof value.relation, cell, "relation") ||
          copy_field(value.object, sizeof value.object, cell, "object"))
      {
         free(arr);
         goto invalid;
      }
      arr[n++] = value;
   }
   cJSON_Delete(root);
   *cells_out = arr;
   *n_out = n;
   return 1;
invalid:
   cJSON_Delete(root);
   return -1;
}
