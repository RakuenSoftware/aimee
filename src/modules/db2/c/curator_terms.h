/* db2/curator_terms.h: corpus terminology normalization.
 *
 * Stage 6 of the corpus pipeline: normalize surface terms to canonical forms.
 */
#ifndef DEC_DB2_CURATOR_TERMS_H
#define DEC_DB2_CURATOR_TERMS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   int db2_corpus_normalize_terms(int64_t doc_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CURATOR_TERMS_H */
