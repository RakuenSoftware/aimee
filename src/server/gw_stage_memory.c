/* gw_stage_memory.c: the ONE memory-injection stage shared by every aimee
 * ingress (universal-gateway P3). See gw_stage_memory.h for the contract.
 *
 * The three render targets are deliberately distinct because they were distinct
 * before P3 and MUST stay byte-identical: the Anthropic path appends a trailing
 * system block (cache-safe), the /v1/responses path merges env+"\n\n"+prior into
 * `instructions`, and the legacy text handlers set the system prompt to the RAW
 * env. Folding the legacy path through ingress_preinject_apply would add a
 * trailing "\n\n" and silently change the provider request bytes. */
#include "gw_stage_memory.h"
#include "ingress_preinject.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <strings.h> /* strcasecmp */
#include <string.h>

/* The Anthropic arm: build the <aimee-context> envelope from this turn's query
 * and fold it into the request's `system` so BOTH the Anthropic-native
 * passthrough (which duplicates `req`) and the translated-provider path (which
 * flattens `req`'s system via anthropic_system_to_text) carry it. Mutates `req`
 * in place, so it must run before translate_request / build_*_provider_body.
 * Appended as a trailing system text block (array form) so a cached system
 * prefix — Claude Code sends cache_control'd system blocks — stays stable and
 * prompt caching still hits. No-op when pre-injection is disabled or recall is
 * empty. Self-contained (ingress_preinject + cJSON only) so the stage TU has no
 * dependency on the anthropic ingress. Non-static so test_anthropic_http.c can
 * exercise it directly (declared in gw_stage_memory.h). */
void messages_apply_preinject(cJSON *req)
{
   char *query =
       ingress_preinject_query_from_messages(cJSON_GetObjectItemCaseSensitive(req, "messages"));
   if (!query)
      return;
   char *env = ingress_preinject_build(query, 0);
   free(query);
   if (!env)
      return;

   cJSON *sys = cJSON_GetObjectItemCaseSensitive(req, "system");
   if (cJSON_IsArray(sys))
   {
      cJSON *blk = cJSON_CreateObject();
      if (blk)
      {
         cJSON_AddStringToObject(blk, "type", "text");
         cJSON_AddStringToObject(blk, "text", env);
         cJSON_AddItemToArray(sys, blk);
      }
   }
   else if (cJSON_IsString(sys) && sys->valuestring && sys->valuestring[0])
   {
      size_t n = strlen(sys->valuestring) + 2 + strlen(env) + 1;
      char *joined = malloc(n);
      if (joined)
      {
         snprintf(joined, n, "%s\n\n%s", sys->valuestring, env);
         cJSON_ReplaceItemInObjectCaseSensitive(req, "system", cJSON_CreateString(joined));
         free(joined);
      }
   }
   else
   {
      /* system absent or empty: the envelope becomes the system prompt. */
      cJSON_DeleteItemFromObjectCaseSensitive(req, "system");
      cJSON_AddStringToObject(req, "system", env);
   }
   free(env);
}

int gw_stage_memory(gw_request_t *r, void *ud)
{
   if (!r || !r->raw)
      return 0;

   switch (r->mem_target)
   {
   case GW_MEM_ANTHROPIC_MESSAGES:
      /* Parity-gated: the Anthropic-native passthrough normally must not perturb
       * the client's cached prefix, so injection is skipped under parity. P5
       * (§2.3) adds an explicit opt-in (r->allow_anthropic_inject, set by the
       * caller from config) to inject on that path too. messages_apply_preinject
       * derives its own query from r->raw.messages and — via
       * ingress_preinject_apply on the string-system path — honors the cache-prefix
       * placement lever. Accounting-neutral (not counted as an intervention). */
      if (!r->parity || r->allow_anthropic_inject)
         messages_apply_preinject(r->raw);
      return 0;

   case GW_MEM_OPENAI_INSTRUCTIONS:
   {
      /* /v1/responses (Codex): derive the query from the chat-shape `messages`
       * (ud), build the envelope, and merge it into raw.instructions as
       * env+"\n\n"+prior — identical to the prior gw_stage_openai_memory. */
      const cJSON *messages = (const cJSON *)ud;
      char *query = ingress_preinject_query_from_messages(messages);
      if (!query)
         return 0; /* no query → no injection (matches the Anthropic arm's guard;
                      ingress_preinject_build also NULL-guards, this is explicit) */
      char *env = ingress_preinject_build(query, 0);
      free(query);
      if (!env)
         return 0;
      cJSON *cur = cJSON_GetObjectItemCaseSensitive(r->raw, "instructions");
      /* ingress_preinject_apply internally honors the cache-prefix placement lever
       * (§2): default prepend, or append after the stable prefix when
       * ingress_cache_placement_enabled. Keeping the choice inside the applier
       * leaves this stage config-free (so every gw_stage_memory consumer links
       * unchanged). */
      char *merged = ingress_preinject_apply(cur ? cur->valuestring : NULL, env);
      free(env);
      if (!merged)
         return 0;
      cJSON_ReplaceItemInObjectCaseSensitive(r->raw, "instructions", cJSON_CreateString(merged));
      free(merged);
      return 1;
   }

   case GW_MEM_OPENAI_SYSTEM_PROMPT:
   {
      /* Legacy text handlers: the RAW envelope becomes the system prompt. NO
       * ingress_preinject_apply — byte-identical to the pre-P3 inline
       * `ingress_preinject_build(query, 0)`. delete+add (rather than replace) so
       * it works whether or not `raw` already carries an `instructions` key. */
      const char *query = (const char *)ud;
      char *env = ingress_preinject_build(query, 0);
      if (!env)
         return 0;
      cJSON_DeleteItemFromObjectCaseSensitive(r->raw, "instructions");
      cJSON_AddStringToObject(r->raw, "instructions", env);
      free(env);
      return 1;
   }
   }

   assert(0 && "gw_stage_memory: unknown mem_target");
   return 0;
}

char *gw_memory_system_prompt(const char *query)
{
   cJSON *raw = cJSON_CreateObject();
   if (!raw)
      return NULL;
   gw_request_t r = {
       .raw = raw,
       .serving_api = GW_API_OPENAI,
       .mem_target = GW_MEM_OPENAI_SYSTEM_PROMPT,
       .parity = 0,
   };
   gw_stage_memory(&r, (void *)query);
   /* NULL (not "") when the stage injected nothing, so callers see exactly what
    * ingress_preinject_build(query, 0) returned before P3. */
   const cJSON *instr = cJSON_GetObjectItemCaseSensitive(raw, "instructions");
   char *out =
       (instr && cJSON_IsString(instr) && instr->valuestring) ? strdup(instr->valuestring) : NULL;
   cJSON_Delete(raw);
   return out;
}

int gw_stage_memory_enabled(void)
{
   /* Default-ON: memory injection runs unless AIMEE_STAGE_MEMORY is an explicit
    * disable token. Full-token match (not first-byte) so "false"/"no" disable but
    * "foo"/"nope" do not. */
   const char *v = getenv("AIMEE_STAGE_MEMORY");
   if (!v || !v[0])
      return 1;
   return !(strcasecmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 ||
            strcasecmp(v, "no") == 0);
}
