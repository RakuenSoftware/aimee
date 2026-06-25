/* kb_client_pdf.c: server-side client for the KB structured-PDF evidence routes
 * (/v1/pdf/search|page|neighbors|structure).
 *
 * Unlike the code-index clients, these routes return purpose-built citation JSON
 * — nested {page_no, bbox:[x0,y0,x1,y1], quote, line_index, content_type}
 * geometry the agent cites verbatim — so we pass the route's JSON body through
 * unchanged rather than round-tripping it through flat C structs (which would
 * drop the citations). Each function returns the malloc'd JSON string the caller
 * frees, or NULL on a parameter/transport/non-2xx failure; *status_out (when
 * non-NULL) carries the route's HTTP status so the caller can craft a useful
 * message (e.g. 413 -> "narrow the query"). URL-escaping mirrors
 * kb_client_index.c: every caller-supplied query-string value is escaped.
 *
 * The access-control invariant lives in the route + DB layer (doc_kind='pdf',
 * quarantine_state<>'pending', project scoping); this client only forwards. */
#include "kb_client.h"
#include "kb_client_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_CLIENT_PDF_READ_TIMEOUT_MS (5 * 1000)

char *kb_client_pdf_search_chunks(const char *query, const char *project, int max_results,
                                  int *status_out)
{
   if (status_out)
      *status_out = -1;
   /* project is REQUIRED: it both scopes the DB query (AND project = ?1) and is
    * what makes the kb_http token-scope gate fire — the gate only denies when
    * the request names a scope, so an un-scoped search would bypass it. See the
    * "project-scope all PDF reads" invariant; this mirrors open_neighbors. */
   if (!query || !query[0] || !project || !project[0])
      return NULL;

   char *query_q = kb_client_query_escape(query);
   char *project_q = kb_client_query_escape(project);
   if (!query_q || !project_q)
   {
      free(query_q);
      free(project_q);
      return NULL;
   }
   if (max_results < 1)
      max_results = 1;

   size_t cap = strlen("/v1/pdf/search?query=&project=&max_results=") + strlen(query_q) +
                strlen(project_q) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(query_q);
      free(project_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/pdf/search?query=%s&project=%s&max_results=%d", query_q, project_q,
            max_results);
   free(query_q);
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_PDF_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_pdf_open_page(const char *project, const char *document_key, int page_no,
                              int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0] || !document_key || !document_key[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   char *dk_q = kb_client_query_escape(document_key);
   if (!project_q || !dk_q)
   {
      free(project_q);
      free(dk_q);
      return NULL;
   }

   size_t cap = strlen("/v1/pdf/page?project=&document_key=&page_no=") + strlen(project_q) +
                strlen(dk_q) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      free(dk_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/pdf/page?project=%s&document_key=%s&page_no=%d", project_q, dk_q,
            page_no);
   free(project_q);
   free(dk_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_PDF_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_pdf_open_neighbors(const char *project, long long chunk_id, int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   if (!project_q)
      return NULL;

   size_t cap = strlen("/v1/pdf/neighbors?project=&chunk_id=") + strlen(project_q) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/pdf/neighbors?project=%s&chunk_id=%lld", project_q, chunk_id);
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_PDF_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_pdf_inspect_structure(const char *project, const char *document_key,
                                      int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0] || !document_key || !document_key[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   char *dk_q = kb_client_query_escape(document_key);
   if (!project_q || !dk_q)
   {
      free(project_q);
      free(dk_q);
      return NULL;
   }

   size_t cap =
       strlen("/v1/pdf/structure?project=&document_key=") + strlen(project_q) + strlen(dk_q) + 8;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      free(dk_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/pdf/structure?project=%s&document_key=%s", project_q, dk_q);
   free(project_q);
   free(dk_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_PDF_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}
