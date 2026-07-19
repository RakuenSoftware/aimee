#include "kb_client.h"
#include "kb_client_internal.h"
#include "kb_doc_hash.h"
#include "aimee.h"
#include "cli_client.h"
#include "cJSON.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *kb_client_docs_error_json(const char *message)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{\"status\":\"error\",\"message\":\"oom\"}");
   cJSON_AddStringToObject(obj, "status", "error");
   cJSON_AddStringToObject(obj, "message", message ? message : "docs manifest failed");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"json failed\"}");
}

static char *kb_client_docs_error_jsonf(const char *fmt, ...)
{
   char msg[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt ? fmt : "docs request failed", ap);
   va_end(ap);
   return kb_client_docs_error_json(msg);
}

static int kb_client_docs_read_file(const char *path, char **bytes_out, int *len_out)
{
   if (!path || !path[0] || !bytes_out || !len_out)
      return -1;

   FILE *fp = fopen(path, "rb");
   if (!fp)
      return -1;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return -1;
   }
   long len = ftell(fp);
   if (len < 0 || len > INT_MAX || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return -1;
   }

   char *bytes = malloc((size_t)len + 1);
   if (!bytes)
   {
      fclose(fp);
      return -1;
   }
   size_t got = len > 0 ? fread(bytes, 1, (size_t)len, fp) : 0;
   if (ferror(fp) || got != (size_t)len)
   {
      free(bytes);
      fclose(fp);
      return -1;
   }
   fclose(fp);
   bytes[len] = '\0';
   *bytes_out = bytes;
   *len_out = (int)len;
   return 0;
}

static int kb_client_docs_multipart_value_ok(const char *value)
{
   if (!value || !value[0])
      return 0;
   return strchr(value, '"') == NULL && strchr(value, '\r') == NULL && strchr(value, '\n') == NULL;
}

static char *kb_client_docs_multipart_body(const char *scope, const char *path, const char *bytes,
                                           int len, char *content_type, size_t content_type_len)
{
   if (!bytes || len <= 0 || !kb_client_docs_multipart_value_ok(path) ||
       memchr(bytes, '\0', (size_t)len))
      return NULL;

   const char *scope_value = (scope && scope[0]) ? scope : "global";
   if (strchr(scope_value, '\r') || strchr(scope_value, '\n'))
      return NULL;

   char hash[KB_DOC_HASH_HEX_LEN + 1];
   kb_doc_content_hash(bytes, len, hash);

   char boundary[96];
   snprintf(boundary, sizeof(boundary), "----AimeeKbDocs%s", hash);
   if (content_type && content_type_len > 0)
   {
      snprintf(content_type, content_type_len, "Content-Type: multipart/form-data; boundary=%s",
               boundary);
   }

   int scope_len = snprintf(NULL, 0,
                            "--%s\r\n"
                            "Content-Disposition: form-data; name=\"scope\"\r\n"
                            "\r\n"
                            "%s\r\n",
                            boundary, scope_value);
   int file_head_len = snprintf(NULL, 0,
                                "--%s\r\n"
                                "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                                "Content-Type: text/markdown\r\n"
                                "\r\n",
                                boundary, path);
   int tail_len = snprintf(NULL, 0, "\r\n--%s--\r\n", boundary);
   if (scope_len < 0 || file_head_len < 0 || tail_len < 0)
      return NULL;

   size_t total = (size_t)scope_len + (size_t)file_head_len + (size_t)len + (size_t)tail_len;
   char *body = malloc(total + 1);
   if (!body)
      return NULL;

   size_t off = 0;
   off += (size_t)snprintf(body + off, total + 1 - off,
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"scope\"\r\n"
                           "\r\n"
                           "%s\r\n",
                           boundary, scope_value);
   off += (size_t)snprintf(body + off, total + 1 - off,
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                           "Content-Type: text/markdown\r\n"
                           "\r\n",
                           boundary, path);
   memcpy(body + off, bytes, (size_t)len);
   off += (size_t)len;
   off += (size_t)snprintf(body + off, total + 1 - off, "\r\n--%s--\r\n", boundary);
   body[off] = '\0';
   return body;
}

static char *kb_client_docs_upload_one_json(const char *scope, const char *path, int *http_status)
{
   if (http_status)
      *http_status = -1;

   char *bytes = NULL;
   int len = 0;
   if (kb_client_docs_read_file(path, &bytes, &len) != 0)
      return NULL;

   char content_type[192];
   char *body =
       kb_client_docs_multipart_body(scope, path, bytes, len, content_type, sizeof(content_type));
   free(bytes);
   if (!body)
      return NULL;

   char *resp = kb_client_v1_post_body_with_type("/v1/docs", body, content_type,
                                                 CLIENT_DEFAULT_TIMEOUT_MS, http_status);
   free(body);
   return resp;
}

static int kb_client_docs_find_path(const char **paths, int path_count, const char *doc_key)
{
   if (!paths || !doc_key)
      return -1;
   for (int i = 0; i < path_count; i++)
   {
      if (paths[i] && strcmp(paths[i], doc_key) == 0)
         return i;
   }
   return -1;
}

static int kb_client_docs_json_int(cJSON *obj, const char *key, int fallback)
{
   cJSON *n = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
   return cJSON_IsNumber(n) ? (int)n->valuedouble : fallback;
}

char *kb_client_docs_manifest_json(const char *scope, const char **paths, int path_count)
{
   if (!paths || path_count <= 0)
      return kb_client_docs_error_json("docs paths required");

   cJSON *root = cJSON_CreateObject();
   cJSON *docs = root ? cJSON_AddArrayToObject(root, "docs") : NULL;
   if (!root || !docs)
   {
      cJSON_Delete(root);
      return kb_client_docs_error_json("oom");
   }
   if (scope && scope[0] && !cJSON_AddStringToObject(root, "scope", scope))
   {
      cJSON_Delete(root);
      return kb_client_docs_error_json("oom");
   }

   for (int i = 0; i < path_count; i++)
   {
      char *bytes = NULL;
      int len = 0;
      if (kb_client_docs_read_file(paths[i], &bytes, &len) != 0)
      {
         cJSON_Delete(root);
         return kb_client_docs_error_jsonf("could not read docs path: %s",
                                           paths[i] ? paths[i] : "");
      }
      char hash[KB_DOC_HASH_HEX_LEN + 1];
      kb_doc_content_hash(bytes, len, hash);
      free(bytes);

      cJSON *doc = cJSON_CreateObject();
      if (!doc)
      {
         cJSON_Delete(root);
         return kb_client_docs_error_json("oom");
      }
      if (!cJSON_AddStringToObject(doc, "doc_key", paths[i] ? paths[i] : "") ||
          !cJSON_AddStringToObject(doc, "content_hash", hash) || !cJSON_AddItemToArray(docs, doc))
      {
         cJSON_Delete(doc);
         cJSON_Delete(root);
         return kb_client_docs_error_json("oom");
      }
   }

   int http_status = -1;
   char *resp =
       kb_client_v1_post_json("/v1/docs/manifest", root, CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(root);
   if (!resp)
      return kb_client_docs_error_json("docs manifest request failed");
   return resp;
}

char *kb_client_docs_upload_json(const char *scope, const char **paths, int path_count)
{
   if (!paths || path_count <= 0)
      return kb_client_docs_error_json("docs paths required");

   cJSON *root = cJSON_CreateObject();
   cJSON *docs = root ? cJSON_AddArrayToObject(root, "docs") : NULL;
   if (!root || !docs)
   {
      cJSON_Delete(root);
      return kb_client_docs_error_json("oom");
   }

   int uploaded = 0;
   int failed = 0;
   for (int i = 0; i < path_count; i++)
   {
      int http_status = -1;
      char *resp = kb_client_docs_upload_one_json(scope, paths[i], &http_status);

      cJSON *doc = cJSON_CreateObject();
      if (!doc)
      {
         free(resp);
         cJSON_Delete(root);
         return kb_client_docs_error_json("oom");
      }
      cJSON_AddStringToObject(doc, "path", paths[i] ? paths[i] : "");
      cJSON_AddNumberToObject(doc, "http_status", http_status);
      if (resp)
      {
         uploaded++;
         cJSON_AddStringToObject(doc, "status", "ok");
         cJSON *resp_json = cJSON_Parse(resp);
         if (resp_json)
            cJSON_AddItemToObject(doc, "response", resp_json);
         else
            cJSON_AddStringToObject(doc, "response", resp);
      }
      else
      {
         failed++;
         cJSON_AddStringToObject(doc, "status", "error");
         cJSON_AddStringToObject(doc, "message", "docs upload request failed");
      }
      free(resp);
      if (!cJSON_AddItemToArray(docs, doc))
      {
         cJSON_Delete(doc);
         cJSON_Delete(root);
         return kb_client_docs_error_json("oom");
      }
   }

   cJSON_AddStringToObject(root, "status", failed ? "error" : "ok");
   cJSON_AddNumberToObject(root, "uploaded", uploaded);
   cJSON_AddNumberToObject(root, "failed", failed);
   cJSON_AddNumberToObject(root, "total", path_count);
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json ? json : kb_client_docs_error_json("json failed");
}

char *kb_client_docs_push_json(const char *scope, const char **paths, int path_count)
{
   if (!paths || path_count <= 0)
      return kb_client_docs_error_json("docs paths required");

   char *manifest_json = kb_client_docs_manifest_json(scope, paths, path_count);
   cJSON *manifest = manifest_json ? cJSON_Parse(manifest_json) : NULL;
   if (!manifest)
   {
      free(manifest_json);
      return kb_client_docs_error_json("docs manifest response invalid");
   }

   cJSON *missing = cJSON_GetObjectItemCaseSensitive(manifest, "missing");
   if (!cJSON_IsArray(missing))
   {
      cJSON_Delete(manifest);
      free(manifest_json);
      return kb_client_docs_error_json("docs manifest missing array required");
   }

   int missing_count = cJSON_GetArraySize(missing);
   const char **missing_paths = NULL;
   if (missing_count > 0)
   {
      missing_paths = calloc((size_t)missing_count, sizeof(*missing_paths));
      if (!missing_paths)
      {
         cJSON_Delete(manifest);
         free(manifest_json);
         return kb_client_docs_error_json("oom");
      }
   }

   int selected = 0;
   for (int i = 0; i < missing_count; i++)
   {
      cJSON *item = cJSON_GetArrayItem(missing, i);
      cJSON *doc_key = cJSON_GetObjectItemCaseSensitive(item, "doc_key");
      if (!cJSON_IsString(doc_key) || !doc_key->valuestring || !doc_key->valuestring[0])
      {
         free(missing_paths);
         cJSON_Delete(manifest);
         free(manifest_json);
         return kb_client_docs_error_json("docs manifest missing doc_key");
      }
      int idx = kb_client_docs_find_path(paths, path_count, doc_key->valuestring);
      if (idx < 0)
      {
         free(missing_paths);
         cJSON_Delete(manifest);
         free(manifest_json);
         return kb_client_docs_error_json("docs manifest referenced unknown path");
      }
      missing_paths[selected++] = paths[idx];
   }

   char *upload_json = NULL;
   cJSON *upload = NULL;
   int uploaded = 0;
   int failed = 0;
   if (selected > 0)
   {
      upload_json = kb_client_docs_upload_json(scope, missing_paths, selected);
      upload = upload_json ? cJSON_Parse(upload_json) : NULL;
      if (!upload)
      {
         free(missing_paths);
         cJSON_Delete(manifest);
         free(manifest_json);
         free(upload_json);
         return kb_client_docs_error_json("docs upload response invalid");
      }
      uploaded = kb_client_docs_json_int(upload, "uploaded", 0);
      failed = kb_client_docs_json_int(upload, "failed", selected);
   }
   free(missing_paths);

   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      cJSON_Delete(upload);
      cJSON_Delete(manifest);
      free(upload_json);
      free(manifest_json);
      return kb_client_docs_error_json("oom");
   }

   cJSON_AddStringToObject(root, "status", failed ? "error" : "ok");
   cJSON_AddNumberToObject(root, "total", path_count);
   cJSON_AddNumberToObject(root, "present", kb_client_docs_json_int(manifest, "present", 0));
   cJSON_AddNumberToObject(root, "missing_count", selected);
   cJSON_AddNumberToObject(root, "uploaded", uploaded);
   cJSON_AddNumberToObject(root, "failed", failed);
   cJSON_AddNumberToObject(root, "skipped", path_count - selected);
   cJSON_AddItemToObject(root, "manifest", manifest);
   if (upload)
      cJSON_AddItemToObject(root, "upload", upload);

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   free(upload_json);
   free(manifest_json);
   return json ? json : kb_client_docs_error_json("json failed");
}
