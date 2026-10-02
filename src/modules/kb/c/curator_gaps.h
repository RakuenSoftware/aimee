/* kb_store/curator_gaps.h: corpus gap detection.
 *
 * Stage 12 of the corpus pipeline: detect undefined-entity and
 * dangling-reference gaps in the corpus.
 */
#ifndef DEC_KB_STORE_CURATOR_GAPS_H
#define DEC_KB_STORE_CURATOR_GAPS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   int kb_store_corpus_detect_gaps(int64_t doc_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_CURATOR_GAPS_H */
