/* kb_surprising_judge.h: §4 surprising-links confirmation stage.
 *
 * The /v1/code/graph/surprising route surfaces STRUCTURAL candidates — file pairs
 * that are semantically close (embedding cosine) yet structurally far / disconnected
 * in the projection graph. This is the relevance gate the proposal calls for: a
 * cheap shared-symbol cross-check plus a single batched LLM-judge call that confirms
 * which candidates are genuine parallel/duplicated logic vs coincidental similarity.
 * The judge runs in-process via the curator Tier-B provider (kb_curator_llm_run). */
#ifndef KB_SURPRISING_JUDGE_H
#define KB_SURPRISING_JUDGE_H

#include <stddef.h>

#include "kb/kb_graph_analytics.h" /* kb_graph_surprising_t */

typedef struct
{
   int sent;           /* 1 if the pair was actually sent to the LLM (paths resolved) */
   int judged;         /* 1 if the LLM returned a verdict for this pair, else 0 */
   int confirmed;      /* LLM verdict: a genuine surprising link (only if judged) */
   int shared_symbols; /* cheap cross-check: # shared symbol names (valid only if sent) */
   char reason[200];   /* short LLM rationale (only if judged) */
} kb_surprising_verdict_t;

/* Confirm up to `n` surprising-link candidates with ONE batched Tier-B LLM call.
 * For each link it resolves both file-node keys to paths, gathers the files' symbol
 * outlines, computes the shared-symbol cross-check, then asks the model which pairs
 * are genuine. `out` is caller-owned and parallel to `links` (n entries, zeroed by
 * the callee). `judge_cmd` is the legacy sidecar fallback (may be NULL). Returns the
 * number of pairs the LLM judged (0 if no Tier-B provider/sidecar is configured — all
 * left judged=0), or -1 on a hard error (request build / unparseable response). Never
 * reorders or drops links; the route decides how to present unconfirmed ones. */
int kb_surprising_judge(const char *judge_cmd, const char *project,
                        const kb_graph_surprising_t *links, int n, kb_surprising_verdict_t *out,
                        char *errbuf, size_t errlen);

#endif /* KB_SURPRISING_JUDGE_H */
