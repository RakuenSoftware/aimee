#include "kb_ocr_sidecar.h"
#include "kb_tsr_sidecar.h"
#include "agent_exec.h"
#include "log.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *response;
static size_t response_length;
static int status = 200, calls;
void aimee_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   (void)module;
   (void)format;
}
int agent_http_post_content_type_bytes(const char *url, const char *auth, const char *type,
                                       const void *body, size_t body_length, char **out,
                                       size_t *out_length, int timeout, const char *extra)
{
   assert(!auth && !extra && !strcmp(type, "application/json"));
   assert(timeout == 30000 || timeout == 60000);
   assert(body && body_length == strlen(body));
   cJSON *request = cJSON_Parse(body);
   assert(request);
   assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(request, "page_no")));
   if (strstr(url, "ocr"))
   {
      assert(
          !strcmp(cJSON_GetObjectItemCaseSensitive(request, "image_base64")->valuestring, "AAH/"));
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "content_type")->valuestring,
                     "image/png"));
   }
   else
      assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(request, "regions")));
   cJSON_Delete(request);
   calls++;
   size_t n = response_length ? response_length : strlen(response);
   *out = malloc(n + 1);
   assert(*out);
   memcpy(*out, response, n);
   (*out)[n] = 0;
   *out_length = n;
   return status;
}
static int ocr(const char *json, size_t length, kb_ocr_line_t **lines, int *n)
{
   const unsigned char png[] = {0, 1, 255};
   response = json;
   response_length = length;
   return kb_ocr_recognize("http://ocr.local", 1, png, sizeof png, lines, n);
}
static void bad_ocr(const char *json, size_t length)
{
   kb_ocr_line_t *lines = (void *)1;
   int n = 99;
   assert(ocr(json, length, &lines, &n) == -1 && !lines && !n);
}
static int tsr(const char *json, size_t length, kb_tsr_cell_t **cells, int *n)
{
   response = json;
   response_length = length;
   return kb_tsr_recognize("http://tsr.local", 1, "[]", cells, n);
}
static void bad_tsr(const char *json, size_t length)
{
   kb_tsr_cell_t *cells = (void *)1;
   int n = 99;
   assert(tsr(json, length, &cells, &n) == -1 && !cells && !n);
}
static char *large_string(const char *field, size_t n, int table)
{
   cJSON *root = cJSON_CreateObject(),
         *array = cJSON_AddArrayToObject(root, table ? "cells" : "lines");
   cJSON *row = cJSON_CreateObject();
   assert(cJSON_AddItemToArray(array, row));
   char *value = malloc(n + 1);
   assert(value);
   memset(value, 'a', n);
   value[n] = 0;
   assert(cJSON_AddStringToObject(row, field, value));
   free(value);
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   assert(json);
   return json;
}
int main(void)
{
   kb_ocr_line_t *lines = NULL;
   int n = 0;
   assert(ocr("{\"lines\":[{\"text\":\"Invoice\",\"x0\":0.1,\"y0\":0.2,\"x1\":0.8,\"y1\":1},{"
              "\"text\":\"\"},{\"text\":\"fallback\"}]}",
              0, &lines, &n) == 1 &&
          n == 2);
   assert(!strcmp(lines[0].text, "Invoice") && lines[0].x0 == 0.1 && lines[0].x1 == 0.8);
   assert(lines[1].x0 == 0 && lines[1].y0 == 0 && lines[1].x1 == 1 && lines[1].y1 == 1);
   kb_ocr_free_lines(lines, n);
   assert(ocr("{\"lines\":[]}", 0, &lines, &n) == 0 && !lines && !n);
   assert(ocr("{\"lines\":[{}, {\"text\":\"\"}]}", 0, &lines, &n) == 0 && !lines && !n);
   const char *bad_lines[] = {"{}",
                              "[]",
                              "{\"lines\":null}",
                              "{\"lines\":4}",
                              "{\"lines\":[null]}",
                              "{\"lines\":[{\"text\":4}]}",
                              "{\"lines\":[{\"text\":\"a\",\"x0\":-0.1}]}",
                              "{\"lines\":[{\"text\":\"a\",\"y1\":1.1}]}",
                              "{\"lines\":[{\"text\":\"a\",\"x0\":0.9,\"x1\":0.1}]}",
                              "{\"lines\":[{\"text\":\"a\",\"y0\":1,\"y1\":0}]}",
                              "{\"lines\":[{\"text\":\"a\",\"x0\":\"0\"}]}",
                              "{\"lines\":[{\"text\":\"a\",\"x1\":1e999}]}",
                              "{\"lines\":[{\"text\":\"ok\"}, {\"text\":true}]}",
                              "{\"lines\":[{\"text\":\"a\",\"text\":\"b\"}]}",
                              "{\"lines\":[{\"text\":\"a\\u0000b\"}]}",
                              "{\"lines\":[]} trailing",
                              "{\"lines\":[]}{}"};
   for (size_t i = 0; i < sizeof bad_lines / sizeof *bad_lines; i++)
      bad_ocr(bad_lines[i], 0);
   const char nul[] = "{\"lines\":[]}\0ignored";
   bad_ocr(nul, sizeof nul - 1);
   char *big = large_string("text", 1024, 0);
   bad_ocr(big, 0);
   free(big);
   big = large_string("text", 1023, 0);
   assert(ocr(big, 0, &lines, &n) == 1 && strlen(lines[0].text) == 1023);
   kb_ocr_free_lines(lines, n);
   free(big);
   cJSON *root = cJSON_CreateObject(), *array = cJSON_AddArrayToObject(root, "lines");
   for (int i = 0; i < 100001; i++)
      assert(cJSON_AddItemToArray(array, cJSON_CreateObject()));
   big = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   assert(big);
   bad_ocr(big, 0);
   free(big);
   kb_tsr_cell_t *cells = NULL;
   assert(tsr("{\"is_table\":true,\"cells\":[{\"row\":2,\"col\":3,\"text\":\"42\",\"confidence\":"
              "100,\"line_index\":0,\"subject\":\"x\"},{}]}",
              0, &cells, &n) == 1 &&
          n == 2);
   assert(cells[0].row == 2 && cells[0].col == 3 && cells[0].confidence == 100 &&
          cells[0].line_index == 0 && !strcmp(cells[0].subject, "x"));
   assert(cells[1].row == 0 && cells[1].col == 0 && cells[1].confidence == 0 &&
          cells[1].line_index == -1 && !cells[1].text[0]);
   kb_tsr_free_cells(cells, n);
   assert(tsr("{\"is_table\":false}", 0, &cells, &n) == 0 && !cells && !n);
   assert(tsr("{\"is_table\":false,\"cells\":[]}", 0, &cells, &n) == 0 && !cells && !n);
   assert(tsr("{\"cells\":[]}", 0, &cells, &n) == 0 && !cells && !n);
   const char *bad_cells[] = {"{}",
                              "[]",
                              "{\"cells\":null}",
                              "{\"is_table\":4,\"cells\":[]}",
                              "{\"is_table\":false,\"cells\":[{}]}",
                              "{\"cells\":[null]}",
                              "{\"cells\":[{\"row\":-1}]}",
                              "{\"cells\":[{\"col\":1.5}]}",
                              "{\"cells\":[{\"row\":2147483648}]}",
                              "{\"cells\":[{\"confidence\":101}]}",
                              "{\"cells\":[{\"confidence\":-1}]}",
                              "{\"cells\":[{\"line_index\":-2}]}",
                              "{\"cells\":[{\"row\":1e999}]}",
                              "{\"cells\":[{\"text\":false}]}",
                              "{\"cells\":[{\"subject\":4}]}",
                              "{\"cells\":[{\"col\":\"1\"}]}",
                              "{\"cells\":[{}, {\"relation\":null}]}",
                              "{\"cells\":[{\"text\":\"a\\u0000b\"}]}",
                              "{\"cells\":[]}tail",
                              "{\"cells\":[],\"cells\":[{}]}"};
   for (size_t i = 0; i < sizeof bad_cells / sizeof *bad_cells; i++)
      bad_tsr(bad_cells[i], 0);
   const char nulcell[] = "{\"cells\":[]}\0tail";
   bad_tsr(nulcell, sizeof nulcell - 1);
   const char *fields[] = {"text", "subject", "relation", "object"};
   const size_t caps[] = {2048, 256, 128, 512};
   for (int i = 0; i < 4; i++)
   {
      big = large_string(fields[i], caps[i], 1);
      bad_tsr(big, 0);
      free(big);
   }
   root = cJSON_CreateObject();
   array = cJSON_AddArrayToObject(root, "cells");
   for (int i = 0; i < 4097; i++)
      assert(cJSON_AddItemToArray(array, cJSON_CreateObject()));
   big = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   assert(big);
   bad_tsr(big, 0);
   free(big);
   int before = calls;
   response = "{\"cells\":[]}";
   response_length = 0;
   cells = (void *)1;
   n = 99;
   assert(kb_tsr_recognize("http://tsr.local", 1, "bad", &cells, &n) == -1 && !cells && !n &&
          calls == before);
   assert(kb_tsr_recognize("http://tsr.local", 1, "{}", &cells, &n) == -1 && calls == before);
   status = 503;
   bad_ocr("{}", 0);
   bad_tsr("{}", 0);
   status = 200;
   puts("OCR/TSR: complete domain frames, faithful rows, geometry/integer limits and atomic "
        "publication passed");
   return 0;
}
