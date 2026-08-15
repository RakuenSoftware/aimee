/* db2/kb_docs.h: aimee-kb ingest API — docs table accessors (DB2).
 *
 * See docs/proposals/pending/aimee-kb-ingest-api-and-corpus-staging.md */
#ifndef DEC_DB2_KB_DOCS_H
#define DEC_DB2_KB_DOCS_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      char content_hash[65];
      char filename[256];
      char scope[64];
      char converter[64];
      char converter_version[128];
      char state[32];
      int review_needed;
      char review_reason[256];
      char created_at[32];
      char updated_at[32];
   } db2_kb_doc_t;

   /* Insert or return existing doc row.
    * If (content_hash, converter, converter_version, scope) already exists,
    * sets *was_existing=1 and returns existing id.
    * Returns doc id (>0) on success, -1 on error. */
   int64_t db2_kb_doc_write(const char *content_hash, const char *filename, const char *scope,
                            const char *converter, const char *converter_version,
                            const char *normalized_text, int *was_existing);

   /* Return 1 when a doc with content_hash exists for scope, 0 when missing,
    * -1 on DB or argument error. */
   int db2_kb_doc_exists_by_hash_scope(const char *content_hash, const char *scope);

   /* Read doc by id. Returns 0 on success, -1 on not found or error. */
   int db2_kb_doc_read(int64_t id, db2_kb_doc_t *out);

   /* Set doc state. If clear_review_needed != 0, sets review_needed=false.
    * review_reason may be NULL (keeps existing). Returns 0 on success. */
   int db2_kb_doc_set_state(int64_t id, const char *state, int clear_review_needed,
                            const char *review_reason);

   /* Delete doc by id. Returns 0 on success, -1 on error / not found. */
   int db2_kb_doc_delete(int64_t id);

   /* List docs in review queue (state='staged', review_needed=true),
    * ordered by id ASC. cursor_id=0 means start from beginning;
    * cursor_id>0 means fetch rows with id > cursor_id.
    * Writes up to max_out rows into out.
    * Returns count written (>=0), -1 on error. */
   int db2_kb_doc_list_review(int limit, int64_t cursor_id, db2_kb_doc_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_KB_DOCS_H */
