/* memmem() is a GNU extension; declare it by requesting _GNU_SOURCE before any
 * libc header. Without this, gcc-12 (the container toolchain) treats memmem as
 * an implicit int-returning function and -Werror fails the build. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_http_ingest.h"
#include "kb_http_ws.h"
#include "kb_doc_hash.h"
#include "kb_ingest_normalize.h"
#include "db2/kb_docs.h"
#include "config.h"
#include "kb_doc_pdf.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int multipart_extract(const char *body, int body_len, const char *field_name, char *text_out,
                             int text_cap, char *filename_out, int filename_cap,
                             const char **content_out, int *content_len)
{
   const char *end_of_line = memchr(body, '\n', body_len < 512 ? body_len : 512);
   if (!end_of_line)
      return 0;
   int bline_len = (int)(end_of_line - body);
   if (bline_len > 0 && body[bline_len - 1] == '\r')
      bline_len--;
   char bline[256];
   if (bline_len <= 0 || bline_len >= (int)sizeof(bline))
      return 0;
   memcpy(bline, body, (size_t)bline_len);
   bline[bline_len] = '\0';

   const char *p = body;
   const char *body_end = body + body_len;
   while (p < body_end)
   {
      const char *bound = memmem(p, (size_t)(body_end - p), bline, (size_t)bline_len);
      if (!bound)
         break;
      p = bound + bline_len;
      if (p >= body_end || *p == '-')
         break;
      if (p < body_end && *p == '\r')
         p++;
      if (p < body_end && *p == '\n')
         p++;
      const char *headers_end = memmem(p, (size_t)(body_end - p), "\r\n\r\n", 4);
      if (!headers_end)
         break;
      char needle[128];
      snprintf(needle, sizeof(needle), "name=\"%s\"", field_name);
      const char *hdr = memmem(p, (size_t)(headers_end - p), needle, strlen(needle));
      if (!hdr)
      {
         p = headers_end + 4;
         continue;
      }
      if (filename_out && filename_cap > 0)
      {
         const char *fn = memmem(p, (size_t)(headers_end - p), "filename=\"", 10);
         if (fn)
         {
            fn += 10;
            const char *fn_end = memchr(fn, '"', (size_t)(headers_end - fn));
            if (fn_end)
            {
               int fnlen = (int)(fn_end - fn);
               if (fnlen >= filename_cap)
                  fnlen = filename_cap - 1;
               memcpy(filename_out, fn, (size_t)fnlen);
               filename_out[fnlen] = '\0';
            }
         }
      }
      const char *content_start = headers_end + 4;
      char end_needle[260];
      snprintf(end_needle, sizeof(end_needle), "\r\n%s", bline);
      const char *content_end =
          memmem(content_start, (size_t)(body_end - content_start), end_needle, strlen(end_needle));
      if (!content_end)
         content_end = body_end;
      int clen = (int)(content_end - content_start);
      if (content_out && content_len)
      {
         *content_out = content_start;
         *content_len = clen;
      }
      if (text_out && text_cap > 0)
      {
         int tlen = clen < text_cap - 1 ? clen : text_cap - 1;
         memcpy(text_out, content_start, (size_t)tlen);
         text_out[tlen] = '\0';
      }
      return 1;
   }
   return 0;
}

static int qparam(const char *qs, const char *key, char *val, int cap)
{
   if (!qs || !key || !val || cap <= 0)
      return 0;
   size_t klen = strlen(key);
   const char *p = qs;
   while (*p)
   {
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         const char *v = p + klen + 1;
         const char *end = strchr(v, '&');
         int vlen = end ? (int)(end - v) : (int)strlen(v);
         if (vlen >= cap)
            vlen = cap - 1;
         memcpy(val, v, (size_t)vlen);
         val[vlen] = '\0';
         return 1;
      }
      const char *next = strchr(p, '&');
      if (!next)
         break;
      p = next + 1;
   }
   return 0;
}

int handle_post_docs(const char *body, int body_len, char *out_buf, int out_cap)
{
   if (!body || body_len <= 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing body\"}");
      return 400;
   }

   char scope[64] = "global";
   multipart_extract(body, body_len, "scope", scope, sizeof(scope), NULL, 0, NULL, NULL);

   char filename[256] = "";
   const char *file_content = NULL;
   int file_len = 0;
   if (!multipart_extract(body, body_len, "file", NULL, 0, filename, sizeof(filename),
                          &file_content, &file_len) ||
       file_len <= 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing file part\"}");
      return 400;
   }

   char content_hash[KB_DOC_HASH_HEX_LEN + 1];
   kb_doc_content_hash(file_content, file_len, content_hash);

   /* structured-PDF routing (Phase 1b): when enabled, a `.pdf` upload is owned by the
    * structured extractor (kb_documents + kb_doc_regions), NOT the legacy flat pdftotext
    * path. A `.pdf` whose bytes are not a real PDF (no %PDF- magic) is REJECTED rather than
    * silently routed to the legacy path — under the flag a `.pdf` must be a PDF, so a
    * spoofed/corrupt header cannot sneak in an untagged document. The upload MUST also carry
    * a valid sensitivity_class. Every reject is a 4xx with NO rows written. */
   {
      config_t cfg;
      config_load(&cfg);
      size_t fn_len = strlen(filename);
      int ext_pdf = fn_len >= 4 && filename[fn_len - 4] == '.' &&
                    (filename[fn_len - 3] | 0x20) == 'p' && (filename[fn_len - 2] | 0x20) == 'd' &&
                    (filename[fn_len - 1] | 0x20) == 'f';
      if (cfg.kb_pdf_ingest_enabled && ext_pdf)
      {
         int magic_pdf = file_len >= 5 && memcmp(file_content, "%PDF-", 5) == 0;
         if (!magic_pdf)
         {
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"file has a .pdf name but is not a PDF (missing %%PDF "
                     "header)\",\"code\":\"not_a_pdf\"}");
            return 400; /* no rows written; not routed to the legacy path */
         }
         char sclass[32] = "";
         multipart_extract(body, body_len, "sensitivity_class", sclass, sizeof(sclass), NULL, 0,
                           NULL, NULL);
         if (!kb_pdf_sensitivity_valid(sclass))
         {
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"a PDF upload requires sensitivity_class in "
                     "{public,internal,restricted}\",\"code\":\"sensitivity_required\"}");
            return 400; /* no rows written */
         }

         /* poppler runs as a separate operator-installed process, with a 30s wall-clock
          * deadline. The `-bbox-layout` XHTML is MUCH larger than the (typically
          * Flate-compressed) PDF bytes — every word is wrapped in verbose
          * `<word xMin=.. yMin=.. xMax=.. yMax=..>` markup — so the output buffer is a
          * generous fixed cap, not a multiple of file_len (the upload itself is already
          * bounded by the KB request-size limit). The cap also bounds a crafted PDF that
          * tries to emit unbounded XHTML (the exec wrapper aborts past it). */
         int xcap = 16 * 1024 * 1024;
         char *xhtml = malloc((size_t)xcap);
         if (!xhtml)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
            return 500;
         }
         if (kb_pdf_exec_bbox_layout((const unsigned char *)file_content, file_len, xhtml, xcap,
                                     30000) != 0)
         {
            free(xhtml);
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"pdf extraction failed\",\"code\":\"converter_error\"}");
            return 422;
         }
         kb_pdf_ingest_stats_t st = {0};
         int rc = kb_doc_pdf_ingest_xhtml(scope, filename, content_hash, xhtml, sclass, &st);
         free(xhtml);
         if (rc < 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"db error\"}");
            return 503;
         }
         /* Phase C: render page crops from the RAW bytes (available only here) into the blob
          * store + kb_doc_assets, when the capability is on. Best-effort + AFTER ingest (the
          * prior assets were dropped by kb_doc_pdf_ingest's re-ingest delete); a missing
          * pdftoppm just yields no assets. Crops are read-time ACL-gated like regions, so even
          * a restricted (pending) doc's crops are safe to render now (withheld until confirm). */
         int assets = 0;
         if (cfg.kb_pdf_assets_enabled)
            assets = kb_doc_pdf_render_assets(scope, filename, sclass,
                                              (const unsigned char *)file_content, file_len);
         kb_ws_publish_invalidation("doc", "global", NULL);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"doc_kind\":\"pdf\",\"chunks\":%d,\"regions\":%d,\"assets\":%d,"
                  "\"sensitivity_class\":\"%s\"}",
                  st.chunks, st.regions, assets, sclass);
         return 201;
      }
   }

   char *normalized = malloc((size_t)(file_len * 2 + 4096));
   if (!normalized)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   char converter[64] = "";
   char converter_version[128] = "";
   int norm_rc = kb_ingest_normalize(filename, file_content, file_len, normalized,
                                     file_len * 2 + 4095, converter, sizeof(converter),
                                     converter_version, sizeof(converter_version));
   if (norm_rc != 0)
   {
      free(normalized);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"normalization failed\",\"code\":\"converter_error\"}");
      return 422;
   }

   int was_existing = 0;
   int64_t doc_id = db2_kb_doc_write(content_hash, filename, scope, converter, converter_version,
                                     normalized, &was_existing);
   free(normalized);

   if (doc_id < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"db error\"}");
      return 503;
   }

   if (!was_existing)
      kb_ws_publish_invalidation("doc", "global", NULL); /* new staged doc */
   snprintf(out_buf, (size_t)out_cap, "{\"doc_id\":%lld,\"state\":\"staged\"}", (long long)doc_id);
   return was_existing ? 200 : 201;
}

int handle_post_docs_manifest(const char *body, int body_len, char *out_buf, int out_cap)
{
   if (!body || body_len <= 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing body\"}");
      return 400;
   }

   char *copy = malloc((size_t)body_len + 1);
   if (!copy)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   memcpy(copy, body, (size_t)body_len);
   copy[body_len] = '\0';

   cJSON *root = cJSON_Parse(copy);
   free(copy);
   if (!root)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"invalid json\"}");
      return 400;
   }

   cJSON *docs = cJSON_GetObjectItemCaseSensitive(root, "docs");
   if (!cJSON_IsArray(docs))
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"docs array required\"}");
      return 400;
   }

   const char *default_scope = "global";
   cJSON *scope_j = cJSON_GetObjectItemCaseSensitive(root, "scope");
   if (cJSON_IsString(scope_j) && scope_j->valuestring && scope_j->valuestring[0])
      default_scope = scope_j->valuestring;

   cJSON *resp = cJSON_CreateObject();
   cJSON *missing = cJSON_AddArrayToObject(resp, "missing");
   int present_count = 0;
   int missing_count = 0;
   int total_count = 0;
   int db_error = 0;

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, docs)
   {
      if (!cJSON_IsObject(item))
         continue;
      cJSON *hash_j = cJSON_GetObjectItemCaseSensitive(item, "content_hash");
      if (!cJSON_IsString(hash_j) || !hash_j->valuestring || !hash_j->valuestring[0])
         continue;
      total_count++;
      const char *scope = default_scope;
      cJSON *item_scope_j = cJSON_GetObjectItemCaseSensitive(item, "scope");
      if (cJSON_IsString(item_scope_j) && item_scope_j->valuestring && item_scope_j->valuestring[0])
         scope = item_scope_j->valuestring;

      int exists = db2_kb_doc_exists_by_hash_scope(hash_j->valuestring, scope);
      if (exists < 0)
      {
         db_error = 1;
         break;
      }
      if (exists)
      {
         present_count++;
         continue;
      }

      cJSON *miss = cJSON_CreateObject();
      cJSON *doc_key_j = cJSON_GetObjectItemCaseSensitive(item, "doc_key");
      if (!cJSON_IsString(doc_key_j))
         doc_key_j = cJSON_GetObjectItemCaseSensitive(item, "path");
      cJSON_AddStringToObject(miss, "doc_key",
                              cJSON_IsString(doc_key_j) ? doc_key_j->valuestring : "");
      cJSON_AddStringToObject(miss, "content_hash", hash_j->valuestring);
      cJSON_AddStringToObject(miss, "scope", scope);
      cJSON_AddItemToArray(missing, miss);
      missing_count++;
   }

   if (db_error)
   {
      cJSON_Delete(resp);
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"db error\"}");
      return 503;
   }

   cJSON_AddNumberToObject(resp, "total", total_count);
   cJSON_AddNumberToObject(resp, "present", present_count);
   cJSON_AddNumberToObject(resp, "missing_count", missing_count);
   char *json = cJSON_PrintUnformatted(resp);
   if (!json)
   {
      cJSON_Delete(resp);
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   snprintf(out_buf, (size_t)out_cap, "%s", json);
   free(json);
   cJSON_Delete(resp);
   cJSON_Delete(root);
   return 200;
}

int handle_get_doc(const char *doc_id, char *out_buf, int out_cap)
{
   int64_t id = (int64_t)atoll(doc_id);
   db2_kb_doc_t doc;
   if (db2_kb_doc_read(id, &doc) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
      return 404;
   }
   snprintf(out_buf, (size_t)out_cap,
            "{\"id\":%lld,\"filename\":\"%s\",\"content_hash\":\"%s\","
            "\"converter\":\"%s\",\"converter_version\":\"%s\","
            "\"scope\":\"%s\",\"state\":\"%s\",\"review_needed\":%s,"
            "\"created_at\":\"%s\"}",
            (long long)doc.id, doc.filename, doc.content_hash, doc.converter, doc.converter_version,
            doc.scope, doc.state, doc.review_needed ? "true" : "false", doc.created_at);
   return 200;
}

int handle_delete_doc(const char *doc_id, char *out_buf, int out_cap)
{
   int64_t id = (int64_t)atoll(doc_id);
   if (db2_kb_doc_delete(id) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
      return 404;
   }
   snprintf(out_buf, (size_t)out_cap, "{\"deleted\":true}");
   return 200;
}

int handle_get_review(const char *query_string, char *out_buf, int out_cap)
{
   int64_t cursor = 0;
   int limit = 10;

   char tmp[64];
   if (qparam(query_string, "cursor", tmp, sizeof(tmp)))
      cursor = (int64_t)atoll(tmp);
   if (qparam(query_string, "limit", tmp, sizeof(tmp)))
   {
      limit = atoi(tmp);
      if (limit <= 0)
         limit = 10;
      if (limit > 50)
         limit = 50;
   }

   db2_kb_doc_t *docs = malloc((size_t)limit * sizeof(db2_kb_doc_t));
   if (!docs)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   int count = db2_kb_doc_list_review(limit, cursor, docs, limit);
   if (count < 0)
   {
      free(docs);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"db error\"}");
      return 503;
   }

   int pos = snprintf(out_buf, (size_t)out_cap, "{\"docs\":[");
   for (int i = 0; i < count && pos < out_cap - 1; i++)
   {
      if (i > 0 && pos < out_cap - 1)
         pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), ",");
      pos += snprintf(out_buf + pos, (size_t)(out_cap - pos),
                      "{\"id\":%lld,\"filename\":\"%s\",\"scope\":\"%s\","
                      "\"state\":\"staged\",\"review_needed\":true,\"created_at\":\"%s\"}",
                      (long long)docs[i].id, docs[i].filename, docs[i].scope, docs[i].created_at);
   }

   if (count == limit)
   {
      pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "],\"next_cursor\":%lld}",
                      (long long)docs[limit - 1].id);
   }
   else
   {
      pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "],\"next_cursor\":null}");
   }

   free(docs);
   return 200;
}
