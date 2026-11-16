/* kb_ocr_sidecar.c: OCR sidecar client. See kb_ocr_sidecar.h. Mirrors kb_tsr_sidecar.c: the
 * in-process byte-counted HTTP transport (no fork, no retention), a response size cap before
 * parse, and -1 on any failure so ingest degrades to asset-only rather than failing. */
#include "kb_ocr_sidecar.h"

#include "agent_exec.h" /* agent_http_post */
#include "cJSON.h"
#include <aimee/ir/aimee_ir.h>
#include <math.h>
#include "log.h"
#include "util.h" /* aimee_base64_encode */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_OCR_TIMEOUT_MS     60000
#define KB_OCR_MAX_LINES      100000
#define KB_OCR_MAX_RESP_BYTES (16 * 1024 * 1024)
/* A single rendered page is bounded by the render harness (RLIMIT_FSIZE 256 MiB); cap the
 * base64 request body well under that so a pathological image can't balloon the request. */
#define KB_OCR_MAX_IMAGE_BYTES (32 * 1024 * 1024)

void kb_ocr_free_lines(kb_ocr_line_t *lines, int n)
{
   (void)n;
   free(lines);
}

static int coordinate(const cJSON *row, const char *key, double fallback, double *out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);
   if (!value)
   {
      *out = fallback;
      return 0;
   }
   if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0 ||
       value->valuedouble > 1)
      return -1;
   *out = value->valuedouble;
   return 0;
}

int kb_ocr_recognize(const char *endpoint, int page_no, const unsigned char *png, int png_len,
                     kb_ocr_line_t **lines_out, int *n_out)
{
   if (lines_out)
      *lines_out = NULL;
   if (n_out)
      *n_out = 0;
   if (!lines_out || !n_out || !endpoint || !endpoint[0] || !png || png_len <= 0 ||
       png_len > KB_OCR_MAX_IMAGE_BYTES)
      return -1;

   /* Request: {"page_no":N, "content_type":"image/png", "image_base64":"..."}. */
   size_t b64cap = aimee_base64_encoded_len((size_t)png_len);
   char *b64 = malloc(b64cap);
   if (!b64)
      return -1;
   aimee_base64_encode(png, (size_t)png_len, b64, b64cap);

   cJSON *req = cJSON_CreateObject();
   if (!req)
   {
      free(b64);
      return -1;
   }
   int fields_ok = cJSON_AddNumberToObject(req, "page_no", page_no) &&
                   cJSON_AddStringToObject(req, "content_type", "image/png");
   /* Guard the load-bearing field: under allocation pressure cJSON_Add could fail, and a
    * request missing the image would have the sidecar "succeed" with no text — silently
    * misclassifying an OOM as a benign no-OCR result. Fail explicitly instead. */
   int img_ok = cJSON_AddStringToObject(req, "image_base64", b64) != NULL;
   free(b64);
   if (!fields_ok || !img_ok)
   {
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
                                          &resp, &response_length, KB_OCR_TIMEOUT_MS, NULL);
   free(body);
   if (status < 200 || status >= 300 || !resp)
   {
      free(resp);
      LOG_WARN("kb_ocr", "OCR sidecar call failed (status=%d)", status);
      return -1;
   }
   if (response_length > KB_OCR_MAX_RESP_BYTES)
   {
      LOG_WARN("kb_ocr", "OCR sidecar response too large (%zu bytes); rejecting", response_length);
      free(resp);
      return -1;
   }

   cJSON *root = aimee_ir_json_parse_frame(resp, response_length);
   free(resp);
   if (!cJSON_IsObject(root))
   {
      LOG_WARN("kb_ocr", "OCR sidecar returned unparseable JSON");
      cJSON_Delete(root);
      return -1;
   }
   const cJSON *lines = cJSON_GetObjectItemCaseSensitive(root, "lines");
   if (!cJSON_IsArray(lines))
      goto invalid;
   int count = cJSON_GetArraySize(lines);
   if (!count)
   {
      cJSON_Delete(root);
      return 0;
   }
   if (count > KB_OCR_MAX_LINES)
      goto invalid;
   kb_ocr_line_t *arr = calloc((size_t)count, sizeof(*arr));
   if (!arr)
      goto invalid;
   int n = 0;
   const cJSON *line;
   cJSON_ArrayForEach(line, lines)
   {
      kb_ocr_line_t value = {0};
      const cJSON *text = cJSON_GetObjectItemCaseSensitive(line, "text");
      if (!cJSON_IsObject(line) ||
          (text && (!cJSON_IsString(text) || strlen(text->valuestring) >= sizeof value.text)) ||
          coordinate(line, "x0", 0, &value.x0) || coordinate(line, "y0", 0, &value.y0) ||
          coordinate(line, "x1", 1, &value.x1) || coordinate(line, "y1", 1, &value.y1) ||
          value.x0 > value.x1 || value.y0 > value.y1)
      {
         free(arr);
         goto invalid;
      }
      if (text)
         memcpy(value.text, text->valuestring, strlen(text->valuestring) + 1);
      /* Empty-text rows carry no quote; preserve the existing skip contract. */
      if (value.text[0])
         arr[n++] = value;
   }
   cJSON_Delete(root);
   if (!n)
   {
      free(arr);
      return 0;
   }
   *lines_out = arr;
   *n_out = n;
   return 1;
invalid:
   cJSON_Delete(root);
   return -1;
}
