/* test_agent_request_build.c -- golden byte-stability for the single canonical
 * request builder (agent_build_request; the legacy per-provider hand-builders were
 * deleted). agent_execute originates provider requests through the IR + per-provider
 * backend; the OUTBOUND bytes must stay stable because Anthropic (and OpenAI) prompt-
 * cache on them. These goldens pin the exact canonical wire per provider so any
 * accidental byte drift in the IR backends fails here.
 *
 * Model-catalog stubs (model_max_output / model_provider_get) keep this a minimal
 * link and make the caps/sampling deterministic; both were consulted identically by
 * every provider path. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "agent_request_build.h"
#include "model_provider.h"
#include "cJSON.h"

int model_max_output(const char *provider, const char *model_id)
{
   (void)provider;
   (void)model_id;
   return 4096;
}
model_provider_t *model_provider_get(const char *name)
{
   (void)name;
   return NULL;
}

static agent_t mk_agent(const char *provider, const char *model)
{
   agent_t a;
   memset(&a, 0, sizeof a);
   snprintf(a.provider, sizeof a.provider, "%s", provider);
   snprintf(a.model, sizeof a.model, "%s", model);
   a.max_tokens = 0; /* unpinned -> derived from the model */
   return a;
}

static void golden(const char *provider, const char *model, const char *expect)
{
   agent_t a = mk_agent(provider, model);
   cJSON *req = agent_build_request(&a, "You are helpful.", "deploy the release", 100, 0.7);
   assert(req && "builder returned NULL");
   char *got = cJSON_PrintUnformatted(req);
   assert(got);
   if (strcmp(got, expect) != 0)
   {
      printf("  BYTE DRIFT provider=%s\n    expect: %s\n    got   : %s\n", provider, expect, got);
      free(got);
      cJSON_Delete(req);
      exit(1);
   }
   printf("  %s egress byte-stable OK\n", provider);
   free(got);
   cJSON_Delete(req);
}

int main(void)
{
   printf("agent_request_build golden:\n");

   /* Anthropic Messages wire: system + user as typed text blocks, max_tokens, then
    * temperature (from model_sampling). This is the HARD byte-identity surface. The
    * system block carries the uniform aimee cache_control policy (mark_cache_prefix):
    * the canonical Anthropic egress caches the stable system prefix on every source. */
   golden("anthropic", "claude-3-5-sonnet",
          "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
          "\"system\":[{\"type\":\"text\",\"text\":\"You are helpful.\","
          "\"cache_control\":{\"type\":\"ephemeral\"}}],"
          "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\","
          "\"text\":\"deploy the release\"}]}],\"temperature\":0.7}");

   /* OpenAI Chat Completions wire. */
   golden("openai", "gpt-4o-mini",
          "{\"model\":\"gpt-4o-mini\",\"max_tokens\":100,"
          "\"messages\":[{\"role\":\"system\",\"content\":\"You are helpful.\"},"
          "{\"role\":\"user\",\"content\":\"deploy the release\"}],\"temperature\":0.7}");

   /* Responses (codex) wire: store=false, stream=true, no max_tokens; canonical input
    * item shape. */
   golden("chatgpt", "gpt-5.5-codex",
          "{\"model\":\"gpt-5.5-codex\",\"store\":false,\"stream\":true,"
          "\"instructions\":\"You are helpful.\","
          "\"input\":[{\"type\":\"message\",\"role\":\"user\","
          "\"content\":[{\"type\":\"input_text\",\"text\":\"deploy the release\"}]}]}");

   printf("all agent_request_build goldens passed\n");
   return 0;
}
