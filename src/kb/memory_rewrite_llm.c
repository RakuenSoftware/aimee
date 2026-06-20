/* memory_rewrite_llm.c: KB-side in-process query-rewrite LLM call (HyDE /
 * decomposition). See memory_rewrite_llm.h.
 *
 * Routes through kb_curator_llm_run so the rewrite reuses the configured
 * per-tier provider (a small fast local model on the curator-llm sidecar)
 * instead of spawning a python sidecar per query. The hypothetical answer is a
 * throwaway retrieval aid, so the light EXTRACT_DOCS tier is the right fit. */
#include "memory_rewrite_llm.h"

#include "kb_curator_llm.h" /* kb_curator_llm_run, KB_CURATOR_STAGE_* */

#include <stddef.h>

#define MEMORY_REWRITE_LLM_OUT_CAP 4096

char *memory_rewrite_llm_inproc(const config_t *cfg, const char *system_prompt, const char *query)
{
   char err[256] = "";
   return kb_curator_llm_run(cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, system_prompt, query ? query : "",
                             "", MEMORY_REWRITE_LLM_OUT_CAP, err, sizeof(err));
}
