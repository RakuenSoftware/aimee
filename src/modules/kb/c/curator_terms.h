/* kb_store/curator_terms.h: corpus terminology normalization.
 *
 * Stage 6 of the corpus pipeline: normalize surface terms to canonical forms.
 */
#ifndef DEC_KB_STORE_CURATOR_TERMS_H
#define DEC_KB_STORE_CURATOR_TERMS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   int kb_store_corpus_normalize_terms(int64_t doc_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_CURATOR_TERMS_H */
