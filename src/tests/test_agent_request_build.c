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
#include <sys/stat.h>
#include <sys/types.h>

#include "agent_request_build.h"
#include "model_provider.h"
#include "model_registry.h"
#include "agent_config.h"
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
/* agent_request_max_tokens() clamps an oversized output cap against the agent's
 * effective context window, falling back to the catalog when no operator
 * override is set. These goldens pin WIRE SHAPE, not capability resolution, so
 * report "no catalog entry" and let the middleware value (unset here) decide. */
const char *agent_catalog_provider(const agent_t *agent)
{
   if (!agent)
      return "";
   return agent->catalog_provider[0] ? agent->catalog_provider : agent->provider;
}
int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   (void)provider;
   (void)model_id;
   (void)out;
   return 0;
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

/* Drive the knob through the REAL config file rather than materialising a config_t.
 * config_t is a secret of the config module (check-config-encapsulation), so a test may
 * not name it -- and going through aimee.yaml is the better test anyway: it exercises the
 * extended_thinking parse in config_sections.c, which a struct poke would skip entirely.
 * AIMEE_NO_CACHE forces a re-read per rewrite so on and off can alternate in-process. */
static char g_home[512];

static void init_config_home(void)
{
   char tmpl[] = "/tmp/aut-xth.XXXXXX";
   const char *d = mkdtemp(tmpl);
   assert(d && "could not create a temp HOME for the config fixture");
   snprintf(g_home, sizeof g_home, "%s", d);
   setenv("HOME", g_home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   char dir[600];
   snprintf(dir, sizeof dir, "%s/.config", g_home);
   assert(mkdir(dir, 0700) == 0);
   snprintf(dir, sizeof dir, "%s/.config/aimee", g_home);
   assert(mkdir(dir, 0700) == 0);
}

static void set_thinking(int enabled, int budget_tokens)
{
   char path[700];
   snprintf(path, sizeof path, "%s/.config/aimee/aimee.yaml", g_home);
   FILE *f = fopen(path, "w");
   assert(f && "could not write the config fixture");
   /* cache_shaping is pinned to its default ON so a thinking assertion never silently
    * also flips the system-block layout the goldens above pin. */
   fprintf(f,
           "cache_shaping:\n"
           "  enabled: true\n"
           "extended_thinking:\n"
           "  enabled: %s\n"
           "  budget_tokens: %d\n",
           enabled ? "true" : "false", budget_tokens);
   fclose(f);
}

static cJSON *build_with_thinking(const char *provider, const char *model, int enabled,
                                  int budget_tokens, int max_tokens)
{
   set_thinking(enabled, budget_tokens);
   agent_t a = mk_agent(provider, model);
   cJSON *req = agent_build_request(&a, "You are helpful.", "deploy the release", max_tokens, 0.7);
   assert(req && "builder returned NULL");
   return req;
}

/* The whole point of the knob: an aimee-originated Anthropic turn asks the model to
 * reason. ir->thinking was previously populated ONLY by an inbound client request, so a
 * turn aimee started itself carried no thinking config at all. */
static void test_thinking_enabled_anthropic(void)
{
   cJSON *req = build_with_thinking("anthropic", "claude-3-5-sonnet", 1, 2048, 100);
   cJSON *th = cJSON_GetObjectItemCaseSensitive(req, "thinking");
   assert(cJSON_IsObject(th) && "thinking config absent on an aimee-originated turn");
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(th, "type")->valuestring, "enabled") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(th, "budget_tokens")->valuedouble == 2048);

   /* Anthropic 4xxs a request pairing thinking with temperature != 1 or any top_p/top_k.
    * model_sampling layers exactly those on AFTER the backend build, so this asserts the
    * normalization that follows it -- not merely that the builder omitted them. */
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble == 1);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_p"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_k"));

   /* budget_tokens must be < max_tokens or the provider rejects it. */
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt) && mt->valuedouble > 2048);
   printf("  anthropic thinking enabled OK (max_tokens=%d)\n", (int)mt->valuedouble);
   cJSON_Delete(req);
}

/* A budget at or above the caller's cap must RAISE the cap, keeping the caller's value as
 * answer headroom -- clearing the provider rule but leaving no room to reply would trade a
 * 4xx for a truncated answer. */
static void test_thinking_budget_exceeds_cap(void)
{
   cJSON *req = build_with_thinking("anthropic", "claude-3-5-sonnet", 1, 4096, 100);
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt));
   assert(mt->valuedouble > 4096 && "max_tokens must exceed budget_tokens");
   printf("  budget over cap raises max_tokens OK (%d)\n", (int)mt->valuedouble);
   cJSON_Delete(req);
}

/* The top_p/top_k strip, actually exercised. The assertions in the test above are vacuous
 * on their own: mk_agent leaves recommended_sampling at 0, so sampling_for_agent returns no
 * row and model_sampling never adds top_p/top_k in the first place. Opting the agent in and
 * naming a model with a curated row (qwen3: top_p 0.95, top_k 20) is what makes
 * model_sampling emit them, so the normalization has something real to remove. The
 * pairing is synthetic -- the curated rows are local delegates -- but the code path that
 * would 4xx against Anthropic is the same one. */
static void test_thinking_strips_curated_sampling(void)
{
   set_thinking(1, 2048);
   agent_t a = mk_agent("anthropic", "qwen3");
   a.recommended_sampling = 1;

   /* Guard the guard: without thinking, the curated row must actually land, or the strip
    * below would be proving nothing again. */
   set_thinking(0, 2048);
   cJSON *plain = agent_build_request(&a, "You are helpful.", "deploy the release", 100, -1);
   assert(plain && cJSON_GetObjectItemCaseSensitive(plain, "top_p") &&
          "curated row did not apply -- the strip assertion would be vacuous");
   assert(cJSON_GetObjectItemCaseSensitive(plain, "top_k"));
   cJSON_Delete(plain);

   set_thinking(1, 2048);
   cJSON *req = agent_build_request(&a, "You are helpful.", "deploy the release", 100, -1);
   assert(req);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_p") && "top_p must be stripped");
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_k") && "top_k must be stripped");
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble == 1);
   printf("  curated top_p/top_k stripped under thinking OK\n");
   cJSON_Delete(req);
}

/* Anthropic-only. `thinking` is not a field on the OpenAI or Responses wires, so emitting
 * it there would be an unknown-parameter error rather than more reasoning. */
static void test_thinking_is_anthropic_only(void)
{
   const char *others[][2] = {{"openai", "gpt-4o-mini"}, {"chatgpt", "gpt-5.5-codex"}};
   for (int i = 0; i < 2; i++)
   {
      cJSON *req = build_with_thinking(others[i][0], others[i][1], 1, 2048, 100);
      assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking"));
      printf("  %s carries no thinking field OK\n", others[i][0]);
      cJSON_Delete(req);
   }
}

/* Default-off must be byte-identical to the pre-change wire: thinking tokens are billed,
 * so an accidental default flip changes what every aimee turn SPENDS. */
static void test_thinking_disabled_is_unchanged(void)
{
   cJSON *req = build_with_thinking("anthropic", "claude-3-5-sonnet", 0, 2048, 100);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking"));
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble == 0.7 && "sampling must be untouched");
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt) && mt->valuedouble == 100);
   printf("  thinking disabled leaves the wire unchanged OK\n");
   cJSON_Delete(req);
}

int main(void)
{
   init_config_home();
   printf("agent_request_build golden:\n");

   /* Anthropic Messages wire: system + user as typed text blocks, max_tokens, then
    * temperature (from model_sampling). This is the HARD byte-identity surface. The
    * system block carries the uniform aimee cache_control policy (mark_cache_prefix):
    * the canonical Anthropic egress caches the stable system prefix on every source.
    * With cache_shaping_enabled default-ON, the builder re-splits + re-marks the system
    * at the <aimee-context> boundary (agent_request_build.c), which re-adds `system`
    * after `messages` — a deterministic, byte-stable layout; key order is not
    * semantically significant to Anthropic and the cache_control content is unchanged. */
   golden("anthropic", "claude-3-5-sonnet",
          "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
          "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\","
          "\"text\":\"deploy the release\"}]}],"
          "\"system\":[{\"type\":\"text\",\"text\":\"You are helpful.\","
          "\"cache_control\":{\"type\":\"ephemeral\"}}],\"temperature\":0.7}");

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

   printf("extended thinking:\n");
   test_thinking_enabled_anthropic();
   test_thinking_budget_exceeds_cap();
   test_thinking_strips_curated_sampling();
   test_thinking_is_anthropic_only();
   test_thinking_disabled_is_unchanged();

   printf("all agent_request_build goldens passed\n");
   return 0;
}
