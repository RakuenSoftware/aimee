/* kb_store/corpus_structural.h: corpus structural analysis helpers.
 *
 * Implements deterministic pieces of the corpus structural-analysis proposal:
 * doc classification, Markdown section trees, document references, and
 * reference staleness marking.
 */
#ifndef DEC_KB_STORE_CORPUS_STRUCTURAL_H
#define DEC_KB_STORE_CORPUS_STRUCTURAL_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CORPUS_DOC_TYPE_LEN       32
#define CORPUS_SECTION_HEADING    256
#define CORPUS_SECTION_PATH       1024
#define CORPUS_REF_TYPE_LEN       32
#define CORPUS_REF_TARGET_LEN     512
#define CORPUS_REF_RESOLUTION_LEN 32

   typedef struct
   {
      int64_t id;
      int64_t doc_id;
      int64_t parent_id;
      int64_t ordinal;
      int depth;
      char heading[CORPUS_SECTION_HEADING];
      char heading_path[CORPUS_SECTION_PATH];
      int64_t span_start;
      int64_t span_end;
      char content_hash[65];
   } kb_store_corpus_section_t;

   typedef struct
   {
      int64_t id;
      int64_t from_doc_id;
      int64_t from_section_id;
      char ref_type[CORPUS_REF_TYPE_LEN];
      char raw_target[CORPUS_REF_TARGET_LEN];
      int64_t to_doc_id;
      char resolution[CORPUS_REF_RESOLUTION_LEN];
      double confidence;
   } kb_store_corpus_reference_t;

   const char *kb_store_corpus_classify_type(const char *filename, const char *normalized_text,
                                             double *confidence_out);
   int kb_store_corpus_classify_doc(int64_t doc_id, const char *operator_id);
   int kb_store_corpus_sections_rebuild(int64_t doc_id);
   int kb_store_corpus_sections_list(int64_t doc_id, kb_store_corpus_section_t *out, int max_out);
   int kb_store_corpus_extract_references(int64_t doc_id);
   int kb_store_corpus_references_list(int64_t from_doc_id, kb_store_corpus_reference_t *out,
                                       int max_out);
   int kb_store_corpus_mark_references_stale_for_doc(int64_t to_doc_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_CORPUS_STRUCTURAL_H */
