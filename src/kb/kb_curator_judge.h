/* kb_curator_judge.h: deep-curator LLM judge sidecar.
 *
 * The resolve_entities NN search produces three outcomes by cosine score:
 *   >= 0.85          a confident merge (no judge needed)
 *   <  0.70          a confident create (no judge needed)
 *   [0.70, 0.85)     ambiguous — too close to create blindly, too far to merge
 *
 * This module adjudicates the ambiguous band with an LLM sidecar: it asks
 * whether the new mention and the nearest existing entity are the same
 * canonical thing. The sidecar reads a JSON request on stdin and writes a JSON
 * response on stdout, mirroring the curator-extract sidecar contract.
 * No DB access from this file. */
#ifndef KB_CURATOR_JUDGE_H
#define KB_CURATOR_JUDGE_H

#include "config.h" /* config_t — for Tier-B provider resolution via kb_curator_llm */

#include <stddef.h>

/* Ask the judge whether `mention` refers to the same canonical entity as
 * `candidate`. Request JSON:
 *   {"task":"same_entity","mention":{"name":...,"context":...},
 *    "candidate":{"name":...},"score":<float>}
 * Expected response JSON: {"same_entity": true|false}.
 *
 * Routes through the §2 dispatch (kb_curator_llm_run): judge is a Tier-B stage,
 * so it uses a configured tier_b.* provider in-process, else the `judge_cmd`
 * sidecar, else errors. `cfg` may be NULL (provider resolution skipped → sidecar
 * only) — the unit tests pass NULL to exercise the shell-sidecar path.
 *
 * Returns 0 and sets *out_same to 0/1 on a clean decision. Returns -1 on any
 * error (no provider and no command, spawn failure, non-zero exit, unparseable
 * output) and leaves *out_same untouched — the caller MUST treat a judge error
 * as "not the same entity" (i.e. create), never as a merge, so a flaky judge can
 * only over-create, never silently collapse distinct entities. */
int kb_curator_judge_same_entity(const config_t *cfg, const char *judge_cmd,
                                 const char *mention_name, const char *mention_context,
                                 const char *candidate_name, double score, int *out_same,
                                 char *errbuf, size_t errlen);

#endif /* KB_CURATOR_JUDGE_H */
