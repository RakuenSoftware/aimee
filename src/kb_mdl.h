/* kb_mdl.h: Two-part Minimum Description Length scoring for synthesis candidate selection.
 * See docs/proposals/accepted/mdl-guided-synthesis-selection.md
 */
#ifndef DEC_KB_MDL_H
#define DEC_KB_MDL_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* MDL_MAX_CANDIDATES: maximum cluster size for within-cluster rank computation. */
#define MDL_MAX_CANDIDATES 32

   typedef struct
   {
      double l_candidate;  /* L(S) — compressed size of candidate under reference coder */
      double l_residual;   /* L(E|S) — residual length after accounting for S */
      double total;        /* l_candidate + l_residual */
      int rank_in_cluster; /* 1 for MDL winner in its cluster, 2 for runner-up, etc. */
   } kb_mdl_score_t;

   /* Compute two-part MDL score for a synthesis candidate `candidate` over
    * evidence bundle `evidence`.  Uses zstd level 3 as reference coder.
    *
    * L(S)   = ZSTD compressed size of candidate.
    * L(E|S) = ZSTD(S || SEP || E) - ZSTD(S) - sep_overhead.
    * total  = L(S) + L(E|S).
    *
    * Lower total = better compression of the evidence = preferred candidate.
    * Returns 0 on success, -1 if compression fails (caller should fall back to
    * attempt-index tie-break). */
   int kb_mdl_score(const char *candidate, const char *evidence, kb_mdl_score_t *out);

   /* Convenience: compute and rank a set of candidates over a shared evidence
    * bundle.  candidates[i] is the i-th candidate string.  Fills scores[n].
    * Returns index of the MDL winner (lowest total), or -1 on error. */
   int kb_mdl_select(const char *const *candidates, int n, const char *evidence,
                     kb_mdl_score_t *scores);

   /* Agreement-aware selection for N-attempt synthesis reducers.  MDL is only
    * allowed to tie-break inside a single cluster whose cluster id appears at
    * least min_agreement times.  If no cluster qualifies, returns -1.  If more
    * than one cluster qualifies, returns -2 so callers can escalate instead of
    * choosing across structurally disagreeing clusters. */
   int kb_mdl_select_agreed_cluster(const char *const *candidates, const char *const *clusters,
                                    int n, const char *evidence, int min_agreement,
                                    kb_mdl_score_t *scores);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_MDL_H */
