/* db2/curator_gaps.h: corpus gap detection.
 *
 * Stage 12 of the corpus pipeline: detect undefined-entity and
 * dangling-reference gaps in the corpus.
 */
#ifndef DEC_DB2_CURATOR_GAPS_H
#define DEC_DB2_CURATOR_GAPS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   int db2_corpus_detect_gaps(int64_t doc_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CURATOR_GAPS_H */
