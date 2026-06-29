/* kb_ocr_sidecar.c: OCR sidecar client. See kb_ocr_sidecar.h. Mirrors kb_tsr_sidecar.c: the
 * in-process agent_http_post transport (no fork, no retention), a response size cap before
 * parse, and -1 on any failure so ingest degrades to asset-only rather than failing. */
#include "kb_ocr_sidecar.h"

#include "agent_exec.h" /* agent_http_post */
#include "cJSON.h"
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

const char *kb_ocr_endpoint(const config_t *cfg)
{
   if (cfg && cfg->ocr_command[0])
      return cfg->ocr_command;
   const char *env = getenv("AIMEE_OCR_URL");
   if (env && env[0])
      return env;
   return "";
}

void kb_ocr_free_lines(kb_ocr_line_t *lines, int n)
{
   (void)n;
   free(lines);
}

static double json_num(cJSON *obj, const char *key, double dflt)
{
   cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(it) ? it->valuedouble : dflt;
}

int kb_ocr_recognize(const char *endpoint, int page_no, const unsigned char *png, int png_len,
                     kb_ocr_line_t **lines_out, int *n_out)
{
   if (lines_out)
      *lines_out = NULL;
   if (n_out)
      *n_out = 0;
   if (!endpoint || !endpoint[0] || !png || png_len <= 0 || png_len > KB_OCR_MAX_IMAGE_BYTES)
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
   cJSON_AddNumberToObject(req, "page_no", page_no);
   cJSON_AddStringToObject(req, "content_type", "image/png");
   /* Guard the load-bearing field: under allocation pressure cJSON_Add could fail, and a
    * request missing the image would have the sidecar "succeed" with no text — silently
    * misclassifying an OOM as a benign no-OCR result. Fail explicitly instead. */
   int img_ok = cJSON_AddStringToObject(req, "image_base64", b64) != NULL;
   free(b64);
   if (!img_ok)
   {
      cJSON_Delete(req);
      return -1;
   }
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   char *resp = NULL;
   int status = agent_http_post(endpoint, NULL, body, &resp, KB_OCR_TIMEOUT_MS, NULL);
   free(body);
   if (status < 200 || status >= 300 || !resp)
   {
      free(resp);
      LOG_WARN("kb_ocr", "OCR sidecar call failed (status=%d)", status);
      return -1;
   }
   if (strlen(resp) > KB_OCR_MAX_RESP_BYTES)
   {
      LOG_WARN("kb_ocr", "OCR sidecar response too large (%zu bytes); rejecting", strlen(resp));
      free(resp);
      return -1;
   }

   cJSON *root = cJSON_Parse(resp);
   free(resp);
   if (!root)
   {
      LOG_WARN("kb_ocr", "OCR sidecar returned unparseable JSON");
      return -1;
   }
   cJSON *lines = cJSON_GetObjectItemCaseSensitive(root, "lines");
   if (!cJSON_IsArray(lines) || cJSON_GetArraySize(lines) == 0)
   {
      cJSON_Delete(root);
      return 0; /* sidecar ran, found no text */
   }
   int count = cJSON_GetArraySize(lines);
   if (count > KB_OCR_MAX_LINES)
      count = KB_OCR_MAX_LINES;
   kb_ocr_line_t *arr = calloc((size_t)count, sizeof(*arr));
   if (!arr)
   {
      cJSON_Delete(root);
      return -1;
   }
   int n = 0;
   cJSON *ln = NULL;
   cJSON_ArrayForEach(ln, lines)
   {
      if (n >= count)
         break;
      cJSON *t = cJSON_GetObjectItemCaseSensitive(ln, "text");
      if (cJSON_IsString(t) && t->valuestring)
         snprintf(arr[n].text, sizeof(arr[n].text), "%s", t->valuestring);
      arr[n].x0 = json_num(ln, "x0", 0.0);
      arr[n].y0 = json_num(ln, "y0", 0.0);
      arr[n].x1 = json_num(ln, "x1", 1.0);
      arr[n].y1 = json_num(ln, "y1", 1.0);
      /* Skip a line with no text (a box with empty content carries no citation value). */
      if (arr[n].text[0])
         n++;
   }
   cJSON_Delete(root);
   if (n == 0)
   {
      free(arr);
      return 0;
   }
   *lines_out = arr;
   *n_out = n;
   return 1;
}
