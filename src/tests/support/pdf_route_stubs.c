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
/* Phase C: kb_doc_assets / blob-store db2 + blob stubs (real SQL/IO in test_kb_doc_pdf.c). */
int db2_kb_doc_asset_open(const char *project, int64_t asset_id, char *blob_ref_out, size_t ref_cap,
                          char *content_type_out, size_t ct_cap)
{
   (void)project;
   (void)asset_id;
   if (blob_ref_out && ref_cap)
      blob_ref_out[0] = '\0';
   if (content_type_out && ct_cap)
      content_type_out[0] = '\0';
   return 0;
}
int db2_kb_doc_assets_list(const char *project, const char *document_key, void *out, int max)
{
   (void)project;
   (void)document_key;
   (void)out;
   (void)max;
   return 0;
}
int kb_blob_store_read(const char *sha, void **out, size_t *n_out)
{
   (void)sha;
   if (out)
      *out = (void *)0;
   if (n_out)
      *n_out = 0;
   return -1;
}
