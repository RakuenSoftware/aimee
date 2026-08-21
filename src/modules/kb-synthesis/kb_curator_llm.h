/* kb_curator_llm.h: curator stage -> LLM dispatch (curator-llm-backend §2b).
 *
 * The single entry point a curator stage calls to run its LLM step. It prefers
 * the configured per-tier provider (§2a resolver + §1 provider_client) and falls
 * back to the legacy `*_command` sidecar when no provider is configured for the
 * stage's tier — so existing sidecar deploys keep working unchanged until an
 * operator sets a provider. Same return contract as kb_curator_sidecar_run: a
 * freshly malloc'd, NUL-terminated response body (caller frees), or NULL with a
 * reason in errbuf. No DB access. */
#ifndef KB_CURATOR_LLM_H
#define KB_CURATOR_LLM_H

#include "kb_curator_provider.h" /* kb_curator_stage_t, config_t */

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run the LLM step for `stage`. Resolution order:
    *   1. a provider configured for the stage's tier (§2a) -> provider_client,
    *      sending `system_prompt` (optional) + `request_json` as the user turn;
    *   2. else `fallback_command` (the legacy sidecar) if non-empty;
    *   3. else NULL with "no provider configured" (the stage stays idle).
    * Returns the response body (caller frees) or NULL. out_cap bounds the sidecar
    * stdout capture ONLY; the provider path returns the full HTTP response body
    * (its length is governed by the model/max_tokens, not out_cap). */
   /* json_schema (optional, provider path only): sent as an OpenAI response_format
    * json_schema so the model returns strict, fence-free JSON in that shape. The
    * sidecar fallback ignores it (it carries its own prompt contract). */
   char *kb_curator_llm_run(const config_t *cfg, kb_curator_stage_t stage,
                            const char *system_prompt, const char *request_json,
                            struct cJSON *json_schema, const char *fallback_command, int out_cap,
                            char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* KB_CURATOR_LLM_H */
