/* kb_http_pdf.h: structured-PDF Phase 2 read routes (citation retrieval). */
#ifndef DEC_KB_HTTP_PDF_H
#define DEC_KB_HTTP_PDF_H 1

/* GET /v1/pdf/search?query=..&project=..&max_results=N — the access-controlled
 * search_chunks tool. Returns matching structured-PDF chunks with line-level
 * {page_no, bbox, quote} citations. Restricted/quarantined PDFs are withheld by the
 * underlying query; the caller's token scope is enforced by kb_http_route_ex before this
 * runs. Writes a JSON body to out_buf and returns the HTTP status. */
int handle_get_pdf_search_route(const char *method, const char *query_string, char *out_buf,
                                int out_cap);

/* POST /v1/pdf/quarantine — §6 quarantine admin. Body: {project, document_key,
 * action:"confirm"|"reject"}. confirm releases a pending restricted PDF (it becomes
 * retrievable via search_chunks); reject purges it. The OWNER-only authorization is enforced
 * by the caller (kb_http_route_ex) before this runs — this handler does body parsing + the
 * state transition only. Writes a JSON body to out_buf and returns the HTTP status. */
int handle_post_pdf_quarantine_route(const char *method, const char *body, int body_len,
                                     char *out_buf, int out_cap);

/* §5 evidence escalation read routes (all GET, all withhold quarantined PDFs; the caller's
 * token scope is enforced by kb_http_route_ex before dispatch):
 *   GET /v1/pdf/page?project=&document_key=&page_no=   - a page's full citation set
 *   GET /v1/pdf/neighbors?chunk_id=                    - the prev/next reading-order chunks
 *   GET /v1/pdf/structure?project=&document_key=       - the document's chunk/page outline */
int handle_get_pdf_page_route(const char *method, const char *query_string, char *out_buf,
                              int out_cap);
int handle_get_pdf_neighbors_route(const char *method, const char *query_string, char *out_buf,
                                   int out_cap);
int handle_get_pdf_structure_route(const char *method, const char *query_string, char *out_buf,
                                   int out_cap);

/* GET /v1/pdf/lookup_table?project=&document_key=[&page_no=] — structured-PDF Phase B.
 * Returns the TSR-recognised table cells for a document (optionally one page), each with
 * {page_no, row, col, text, tsr_confidence}, plus a tsr_status marker
 * (ran | not_a_table | unavailable) so a caller can tell "no cells" from "TSR never ran".
 * Gated by the full PDF ACL (doc_kind='pdf' + quarantine + project) via a join to the
 * authoritative kb_documents row; a guessed/foreign/withheld document_key returns empty.
 * The caller's token scope is enforced by kb_http_route_ex before this runs. */
int handle_get_pdf_lookup_table_route(const char *method, const char *query_string, char *out_buf,
                                      int out_cap);

#endif /* DEC_KB_HTTP_PDF_H */
