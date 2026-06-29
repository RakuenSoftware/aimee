/* kb_http_pdf.c: structured-PDF Phase 2 read route — search_chunks (citation retrieval).
 * Lexical (case-insensitive content match) over PDF chunks, returning line-level
 * {page_no, bbox, quote} citations from kb_doc_regions. The DB query always excludes
 * non-PDF rows and withholds quarantine_state='pending' (restricted) documents; the
 * caller's token scope is already enforced by kb_http_route_ex before dispatch. */
#include "kb_http_pdf.h"

#include "cJSON.h"
#include "db2/kb_payload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDF_MAX_CHUNKS 10
/* A chunk is bounded to KB_PDF_MAX_CHUNK_LINES (100) lines = 100 regions; 200 is headroom. */
#define PDF_MAX_REGIONS 200

/* URL-decoded value of query param `key` from `qs` (handles %XX and '+'). Returns 1 if
 * found. */
static int pdf_qparam(const char *qs, const char *key, char *out, int outsz)
{
   if (!qs || !key || !out || outsz <= 0)
      return 0;
   out[0] = '\0';
   int klen = (int)strlen(key);
   for (const char *p = qs; *p; p++)
   {
      if ((p == qs || p[-1] == '&') && strncmp(p, key, (size_t)klen) == 0 && p[klen] == '=')
      {
         p += klen + 1;
         int i = 0;
         while (*p && *p != '&' && i < outsz - 1)
         {
            if (*p == '%' && p[1] && p[2])
            {
               char hex[3] = {p[1], p[2], 0};
               out[i++] = (char)strtol(hex, NULL, 16);
               p += 3;
            }
            else if (*p == '+')
            {
               out[i++] = ' ';
               p++;
            }
            else
            {
               out[i++] = *p++;
            }
         }
         out[i] = '\0';
         return 1;
      }
   }
   return 0;
}

int handle_get_pdf_search_route(const char *method, const char *query_string, char *out_buf,
                                int out_cap)
{
   if (!method || strcmp(method, "GET") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }

   char query[512] = "", project[128] = "", maxs[16] = "";
   if (!pdf_qparam(query_string, "query", query, sizeof(query)) || !query[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing query parameter\"}");
      return 400;
   }
   pdf_qparam(query_string, "project", project, sizeof(project));
   int max = PDF_MAX_CHUNKS;
   if (pdf_qparam(query_string, "max_results", maxs, sizeof(maxs)))
   {
      max = atoi(maxs);
      if (max < 1)
         max = 1;
      if (max > PDF_MAX_CHUNKS)
         max = PDF_MAX_CHUNKS;
   }

   db2_kb_pdf_chunk_t *chunks = malloc((size_t)PDF_MAX_CHUNKS * sizeof(*chunks));
   db2_kb_pdf_region_t *regs = malloc((size_t)PDF_MAX_REGIONS * sizeof(*regs));
   if (!chunks || !regs)
   {
      free(chunks);
      free(regs);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   db2_kb_answerability_t ans;
   int n = db2_kb_pdf_search_chunks(project[0] ? project : NULL, query, max, chunks, &ans);

   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "chunks");
   for (int i = 0; i < n; i++)
   {
      cJSON *c = cJSON_CreateObject();
      cJSON_AddNumberToObject(c, "chunk_id", (double)chunks[i].chunk_id);
      cJSON_AddStringToObject(c, "document_key", chunks[i].document_key);
      cJSON_AddNumberToObject(c, "page_start", chunks[i].page_start);
      cJSON_AddNumberToObject(c, "page_end", chunks[i].page_end);
      cJSON_AddStringToObject(c, "content", chunks[i].content);
      cJSON_AddStringToObject(c, "sensitivity_class", chunks[i].sensitivity_class);
      /* Phase A2: relevance + which leg matched. */
      cJSON_AddNumberToObject(c, "score", chunks[i].score);
      cJSON_AddStringToObject(c, "matched_via", chunks[i].matched_vector ? "vector" : "lexical");

      cJSON *cits = cJSON_AddArrayToObject(c, "citations");
      int rn = db2_kb_doc_regions_for_chunk(chunks[i].chunk_id, regs, PDF_MAX_REGIONS);
      /* Phase A2: has_citation is the candidate→region LEFT-JOIN flag — a candidate whose
       * regions are not (yet) present degrades to has_citation=false with an empty
       * citations array instead of being silently dropped. */
      cJSON_AddBoolToObject(c, "has_citation", rn > 0 ? 1 : 0);
      for (int j = 0; j < rn; j++)
      {
         cJSON *cit = cJSON_CreateObject();
         cJSON_AddNumberToObject(cit, "page_no", regs[j].page_no);
         cJSON *bbox = cJSON_AddArrayToObject(cit, "bbox");
         cJSON_AddItemToArray(bbox, cJSON_CreateNumber(regs[j].x0));
         cJSON_AddItemToArray(bbox, cJSON_CreateNumber(regs[j].y0));
         cJSON_AddItemToArray(bbox, cJSON_CreateNumber(regs[j].x1));
         cJSON_AddItemToArray(bbox, cJSON_CreateNumber(regs[j].y1));
         cJSON_AddStringToObject(cit, "quote", regs[j].quote);
         cJSON_AddNumberToObject(cit, "line_index", regs[j].line_index);
         cJSON_AddStringToObject(cit, "content_type", regs[j].content_type);
         cJSON_AddItemToArray(cits, cit);
      }
      cJSON_AddItemToArray(arr, c);
   }
   cJSON_AddNumberToObject(root, "total", n);

   /* Phase A3: the per-query-over-corpus answerability judgment. This is a KB-side SHARED
    * signal and is deliberately a DISTINCT field from any server-side per-user confidence
    * tier — clients must not conflate the two. */
   cJSON *aobj = cJSON_AddObjectToObject(root, "answerability");
   cJSON_AddNumberToObject(aobj, "score", ans.score);
   cJSON_AddStringToObject(aobj, "label", ans.label);
   cJSON *ains = cJSON_AddObjectToObject(aobj, "inputs");
   cJSON_AddNumberToObject(ains, "top_score", ans.top_score);
   cJSON_AddNumberToObject(ains, "coverage", ans.coverage);
   cJSON_AddNumberToObject(ains, "saturation", ans.saturation);
   cJSON_AddNumberToObject(ains, "table_facts", ans.table_facts);

   char *s = cJSON_PrintUnformatted(root);
   int status = 200;
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      status = 500;
   }
   else if (strlen(s) >= (size_t)out_cap)
   {
      /* Never return truncated (invalid) JSON: signal the caller to narrow the request. */
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"result too large; reduce max_results or narrow the "
               "query\",\"code\":\"result_too_large\"}");
      status = 413;
   }
   else
   {
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   }
   free(s);
   cJSON_Delete(root);
   free(chunks);
   free(regs);
   return status;
}

int handle_post_pdf_quarantine_route(const char *method, const char *body, int body_len,
                                     char *out_buf, int out_cap)
{
   (void)body_len;
   if (!method || strcmp(method, "POST") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }

   cJSON *req = body ? cJSON_Parse(body) : NULL;
   const cJSON *jproj = req ? cJSON_GetObjectItemCaseSensitive(req, "project") : NULL;
   const cJSON *jdk = req ? cJSON_GetObjectItemCaseSensitive(req, "document_key") : NULL;
   const cJSON *jact = req ? cJSON_GetObjectItemCaseSensitive(req, "action") : NULL;
   if (!cJSON_IsString(jproj) || !jproj->valuestring[0] || !cJSON_IsString(jdk) ||
       !jdk->valuestring[0] || !cJSON_IsString(jact))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"bad request: project, document_key, and action are "
               "required\",\"code\":\"bad_request\"}");
      return 400;
   }

   const char *project = jproj->valuestring;
   const char *dk = jdk->valuestring;
   const char *action = jact->valuestring;
   int rc;
   if (strcmp(action, "confirm") == 0)
      rc = db2_kb_pdf_quarantine_confirm(project, dk);
   else if (strcmp(action, "reject") == 0)
      rc = db2_kb_pdf_quarantine_reject(project, dk);
   else
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"action must be 'confirm' or 'reject'\",\"code\":\"bad_request\"}");
      return 400;
   }

   /* Snapshot before freeing req (dk points into it). */
   char dk_copy[1024];
   snprintf(dk_copy, sizeof(dk_copy), "%s", dk);
   char action_copy[16];
   snprintf(action_copy, sizeof(action_copy), "%s", action);
   cJSON_Delete(req);

   if (rc < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"db error\"}");
      return 503;
   }
   if (rc == 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"no pending PDF for that document_key\",\"code\":\"not_found\"}");
      return 404;
   }
   snprintf(out_buf, (size_t)out_cap, "{\"document_key\":\"%s\",\"action\":\"%s\",\"chunks\":%d}",
            dk_copy, action_copy, rc);
   return 200;
}

/* Emit a cJSON root to out_buf, returning 200, 500 (oom), or 413 (too large for the buffer —
 * never truncated/invalid JSON). Frees `root`. */
static int pdf_emit_json(cJSON *root, char *out_buf, int out_cap)
{
   char *s = cJSON_PrintUnformatted(root);
   int status = 200;
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      status = 500;
   }
   else if (strlen(s) >= (size_t)out_cap)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"result too large\",\"code\":\"result_too_large\"}");
      status = 413;
   }
   else
   {
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   }
   free(s);
   cJSON_Delete(root);
   return status;
}

static void pdf_add_citation(cJSON *arr, const db2_kb_pdf_region_t *r)
{
   cJSON *cit = cJSON_CreateObject();
   cJSON_AddNumberToObject(cit, "page_no", r->page_no);
   cJSON *bbox = cJSON_AddArrayToObject(cit, "bbox");
   cJSON_AddItemToArray(bbox, cJSON_CreateNumber(r->x0));
   cJSON_AddItemToArray(bbox, cJSON_CreateNumber(r->y0));
   cJSON_AddItemToArray(bbox, cJSON_CreateNumber(r->x1));
   cJSON_AddItemToArray(bbox, cJSON_CreateNumber(r->y1));
   cJSON_AddStringToObject(cit, "quote", r->quote);
   cJSON_AddNumberToObject(cit, "line_index", r->line_index);
   cJSON_AddStringToObject(cit, "content_type", r->content_type);
   cJSON_AddItemToArray(arr, cit);
}

int handle_get_pdf_page_route(const char *method, const char *query_string, char *out_buf,
                              int out_cap)
{
   if (!method || strcmp(method, "GET") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }
   char project[128] = "", dk[1024] = "", pages[16] = "";
   if (!pdf_qparam(query_string, "project", project, sizeof(project)) || !project[0] ||
       !pdf_qparam(query_string, "document_key", dk, sizeof(dk)) || !dk[0] ||
       !pdf_qparam(query_string, "page_no", pages, sizeof(pages)) || !pages[0])
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"project, document_key, and page_no are required\"}");
      return 400;
   }
   db2_kb_pdf_region_t *regs = malloc((size_t)PDF_MAX_REGIONS * sizeof(*regs));
   if (!regs)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int n = db2_kb_pdf_open_page(project, dk, atoi(pages), regs, PDF_MAX_REGIONS);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "document_key", dk);
   cJSON_AddNumberToObject(root, "page_no", atoi(pages));
   cJSON *cits = cJSON_AddArrayToObject(root, "citations");
   for (int i = 0; i < n; i++)
      pdf_add_citation(cits, &regs[i]);
   cJSON_AddNumberToObject(root, "total", n);
   free(regs);
   return pdf_emit_json(root, out_buf, out_cap);
}

int handle_get_pdf_neighbors_route(const char *method, const char *query_string, char *out_buf,
                                   int out_cap)
{
   if (!method || strcmp(method, "GET") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }
   char project[128] = "", ids[32] = "";
   /* project is REQUIRED: it scopes the lookup AND makes the kb_http_route_ex token-scope gate
    * fire (that gate only acts when the request names a project/scope), closing a cross-scope
    * chunk-id enumeration leak. */
   if (!pdf_qparam(query_string, "project", project, sizeof(project)) || !project[0] ||
       !pdf_qparam(query_string, "chunk_id", ids, sizeof(ids)) || !ids[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"project and chunk_id are required\"}");
      return 400;
   }
   db2_kb_pdf_chunk_t neigh[4];
   int n = db2_kb_pdf_open_neighbors(project, (int64_t)atoll(ids), neigh, 4);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddNumberToObject(root, "chunk_id", (double)atoll(ids));
   cJSON *arr = cJSON_AddArrayToObject(root, "neighbors");
   for (int i = 0; i < n; i++)
   {
      cJSON *c = cJSON_CreateObject();
      cJSON_AddNumberToObject(c, "chunk_id", (double)neigh[i].chunk_id);
      cJSON_AddStringToObject(c, "document_key", neigh[i].document_key);
      cJSON_AddNumberToObject(c, "page_start", neigh[i].page_start);
      cJSON_AddNumberToObject(c, "page_end", neigh[i].page_end);
      cJSON_AddStringToObject(c, "content", neigh[i].content);
      cJSON_AddStringToObject(c, "sensitivity_class", neigh[i].sensitivity_class);
      cJSON_AddItemToArray(arr, c);
   }
   cJSON_AddNumberToObject(root, "total", n);
   return pdf_emit_json(root, out_buf, out_cap);
}

#define PDF_MAX_OUTLINE 2000

int handle_get_pdf_structure_route(const char *method, const char *query_string, char *out_buf,
                                   int out_cap)
{
   if (!method || strcmp(method, "GET") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }
   char project[128] = "", dk[1024] = "";
   if (!pdf_qparam(query_string, "project", project, sizeof(project)) || !project[0] ||
       !pdf_qparam(query_string, "document_key", dk, sizeof(dk)) || !dk[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"project and document_key are required\"}");
      return 400;
   }
   db2_kb_pdf_outline_t *ol = malloc((size_t)PDF_MAX_OUTLINE * sizeof(*ol));
   if (!ol)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int n = db2_kb_pdf_inspect_structure(project, dk, ol, PDF_MAX_OUTLINE);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "document_key", dk);
   cJSON *arr = cJSON_AddArrayToObject(root, "chunks");
   for (int i = 0; i < n; i++)
   {
      cJSON *c = cJSON_CreateObject();
      cJSON_AddNumberToObject(c, "chunk_index", ol[i].chunk_index);
      cJSON_AddNumberToObject(c, "page_start", ol[i].page_start);
      cJSON_AddNumberToObject(c, "page_end", ol[i].page_end);
      cJSON_AddStringToObject(c, "heading_path", ol[i].heading_path);
      cJSON_AddItemToArray(arr, c);
   }
   cJSON_AddNumberToObject(root, "total", n);
   free(ol);
   return pdf_emit_json(root, out_buf, out_cap);
}
