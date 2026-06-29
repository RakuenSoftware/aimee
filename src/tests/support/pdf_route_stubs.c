/* pdf_route_stubs.c: link-only stubs for the structured-PDF db2 layer, used by
 * test_kb_http_routes (which links the real kb_http_pdf.o route handlers but stubs the
 * db2 layer below them). The real SQL is exercised against the sqlite shim in
 * test_kb_doc_pdf.c. void* args avoid pulling kb_payload.h into the stub TU. */
#include <stddef.h>
#include <stdint.h>

int db2_kb_pdf_search_chunks(const char *project, const char *query, int max, void *out, void *a)
{
   (void)project;
   (void)query;
   (void)max;
   (void)out;
   (void)a;
   return 0;
}
int db2_kb_doc_regions_for_chunk(int64_t chunk_id, void *out, int max)
{
   (void)chunk_id;
   (void)out;
   (void)max;
   return 0;
}
int db2_kb_table_cells_lookup(const char *project, const char *document_key, int page_no, void *out,
                              int max)
{
   (void)project;
   (void)document_key;
   (void)page_no;
   (void)out;
   (void)max;
   return 0;
}
int db2_kb_pdf_tsr_state(const char *project, const char *document_key, char *out, size_t out_len)
{
   (void)project;
   (void)document_key;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}
int db2_kb_pdf_quarantine_confirm(const char *project, const char *document_key)
{
   (void)project;
   (void)document_key;
   return 0;
}
int db2_kb_pdf_quarantine_reject(const char *project, const char *document_key)
{
   (void)project;
   (void)document_key;
   return 0;
}
int db2_kb_pdf_open_page(const char *project, const char *document_key, int page_no, void *out,
                         int max)
{
   (void)project;
   (void)document_key;
   (void)page_no;
   (void)out;
   (void)max;
   return 0;
}
int db2_kb_pdf_open_neighbors(const char *project, int64_t chunk_id, void *out, int max)
{
   (void)project;
   (void)chunk_id;
   (void)out;
   (void)max;
   return 0;
}
int db2_kb_pdf_inspect_structure(const char *project, const char *document_key, void *out, int max)
{
   (void)project;
   (void)document_key;
   (void)out;
   (void)max;
   return 0;
}
