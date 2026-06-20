/* memory_rewrite_llm.c: KB-side in-process query-rewrite LLM call (HyDE /
 * decomposition). See memory_rewrite_llm.h.
 *
 * Resolves the curator per-tier provider (a small fast local model on the
 * curator-llm sidecar) and calls it directly, instead of spawning a python
 * sidecar per query. A HyDE answer is 1-3 sentences, so the generation is
 * capped: with no cap an unconstrained local model occasionally runs away (a
 * 400+ token / 30s+ completion observed), which stalls retrieval. Capping bounds
 * the tail without affecting other curator stages (which call kb_curator_llm_run
 * with their own, uncapped, budget). */
#include "memory_rewrite_llm.h"

#include "cJSON.h"
#include "kb_curator_provider.h" /* kb_curator_provider_for_stage, KB_CURATOR_STAGE_* */
#include "provider_client.h"     /* provider_def_t, provider_client_complete */

#include <stddef.h>
#include <string.h>

/* A hyde answer / sub-question list is short; bound generation so a rambling
 * local model can't stall the recall path. Generous enough for a 1-3 sentence
 * hypothetical answer plus a few sub-questions. */
#define MEMORY_REWRITE_MAX_TOKENS 160

static cJSON *rewrite_messages(const char *system_prompt, const char *query)
{
   cJSON *msgs = cJSON_CreateArray();
   if (!msgs)
      return NULL;
   if (system_prompt && system_prompt[0])
   {
      cJSON *s = cJSON_CreateObject();
      if (!s)
      {
         cJSON_Delete(msgs);
         return NULL;
      }
      cJSON_AddStringToObject(s, "role", "system");
      cJSON_AddStringToObject(s, "content", system_prompt);
      cJSON_AddItemToArray(msgs, s);
   }
   cJSON *u = cJSON_CreateObject();
   if (!u)
   {
      cJSON_Delete(msgs);
      return NULL;
   }
   cJSON_AddStringToObject(u, "role", "user");
   cJSON_AddStringToObject(u, "content", query ? query : "");
   cJSON_AddItemToArray(msgs, u);
   return msgs;
}

char *memory_rewrite_llm_inproc(const config_t *cfg, const char *system_prompt, const char *query)
{
   provider_def_t def;
   memset(&def, 0, sizeof(def));
   /* The rewrite shares the typed-fact extractor's light tier. No provider
    * configured -> no in-process rewrite (caller falls back). */
   if (!cfg || !kb_curator_provider_for_stage(cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, &def))
      return NULL;
   if (def.max_tokens <= 0 || def.max_tokens > MEMORY_REWRITE_MAX_TOKENS)
      def.max_tokens = MEMORY_REWRITE_MAX_TOKENS;

   cJSON *msgs = rewrite_messages(system_prompt, query);
   if (!msgs)
      return NULL;

   provider_completion_t out;
   memset(&out, 0, sizeof(out));
   char err[256] = "";
   int rc = provider_client_complete(&def, msgs, NULL, &out, err, sizeof(err));
   cJSON_Delete(msgs);
   if (rc != 0)
   {
      provider_completion_free(&out);
      return NULL;
   }
   char *content = out.content; /* transfer ownership to caller */
   out.content = NULL;
   provider_completion_free(&out);
   return content;
}
