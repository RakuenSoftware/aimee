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
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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
/* Capability stub, driven per-test. The builder now asks the catalog whether a
 * model accepts Anthropic's ADAPTIVE thinking shape, so what this returns is the
 * decision under test: `g_caps_flags` is what a provider (or an operator's
 * capability override) would have reported, and g_caps_known == 0 is the
 * ordinary "nobody has said" case that must fail closed. */
static unsigned g_caps_flags;
static int g_caps_known;

int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   (void)provider;
   (void)model_id;
   if (!g_caps_known || !out)
      return 0;
   memset(out, 0, sizeof(*out));
   out->flags = g_caps_flags;
   out->context_window = 200000;
   out->max_output = 8192;
   return 1;
}

static void declare_caps(unsigned flags)
{
   g_caps_flags = flags;
   g_caps_known = 1;
}

static void clear_caps(void)
{
   g_caps_flags = 0;
   g_caps_known = 0;
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
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aut-xth.XXXXXX", platform_tmpdir());
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

/* The knob asks a model to reason -- but ONLY a model that says it accepts this
 * shape. The config it emits is `{"type":"adaptive"}`: Anthropic removed
 * {"type":"enabled", budget_tokens: N}, which is a 400 on Opus 4.7/4.8/5,
 * Sonnet 5 and Fable 5 and survives only on the 4.6 generation. */
static void test_thinking_adaptive_when_model_supports_it(void)
{
   declare_caps(MODEL_CAP_REASONING | MODEL_CAP_THINKING_ADAPTIVE);
   cJSON *req = build_with_thinking("anthropic", "claude-sonnet-4-6", 1, 2048, 100);
   cJSON *th = cJSON_GetObjectItemCaseSensitive(req, "thinking");
   assert(cJSON_IsObject(th) && "thinking config absent on an aimee-originated turn");
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(th, "type")->valuestring, "adaptive") == 0);
   /* The retired form must not reappear: budget_tokens is what 400s. */
   assert(!cJSON_GetObjectItemCaseSensitive(th, "budget_tokens"));

   /* Sampling is REMOVED, not pinned to temperature=1 as it used to be. Every
    * model reaching this branch is 4.6-or-later, and on 4.7+ any of
    * temperature/top_p/top_k is itself a 400 -- so the old "fix" turned one
    * rejection into two. Removal is valid across the whole range. */
   assert(!cJSON_GetObjectItemCaseSensitive(req, "temperature"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_p"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_k"));

   /* max_tokens is the caller's, untouched: adaptive has no budget to fit under,
    * so the old "raise the cap to clear budget_tokens" arithmetic is gone. */
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt) && mt->valuedouble == 100);
   printf("  adaptive thinking emitted for a capable model OK\n");
   cJSON_Delete(req);
   clear_caps();
}

/* FAIL CLOSED. A model nobody has said accepts adaptive thinking gets no
 * thinking config at all. Sending nothing costs a missed chance to reason;
 * sending the wrong shape costs a 400 the operator sees as an agent failure. */
static void test_thinking_absent_when_capability_unknown(void)
{
   clear_caps();
   cJSON *req = build_with_thinking("anthropic", "some-unknown-model-qqq", 1, 2048, 100);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking"));
   /* And sampling is left exactly as configured, because nothing was stripped. */
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble == 0.7);
   printf("  unknown capability emits no thinking OK\n");
   cJSON_Delete(req);
}

/* THE PREDICATE BUG. `is_anth` is the WIRE FORMAT, not the vendor: MiniMax and
 * Moonshot both ship Anthropic-compatible endpoints and are configured with
 * provider="anthropic", catalog_provider="minimax". Enabling this knob used to
 * send Anthropic thinking config to them and to nothing else, since the Claude
 * seat is a provider-CLI agent that never reaches this builder. The capability
 * check excludes them without having to name them. */
static void test_thinking_skips_anthropic_compatible_third_party(void)
{
   /* The vendor is catalogued and reasoning-capable, but has never advertised
    * Anthropic's adaptive thinking -- because it is not an Anthropic model. */
   declare_caps(MODEL_CAP_REASONING | MODEL_CAP_TOOLS);
   agent_t a = mk_agent("anthropic", "MiniMax-M3");
   snprintf(a.catalog_provider, sizeof a.catalog_provider, "minimax");
   set_thinking(1, 2048);
   cJSON *req = agent_build_request(&a, "You are helpful.", "deploy the release", 100, 0.7);
   assert(req);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking") &&
          "a third-party model on the anthropic wire must not receive thinking config");
   printf("  anthropic-compatible third party gets no thinking OK\n");
   cJSON_Delete(req);
   clear_caps();
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
   test_thinking_adaptive_when_model_supports_it();
   test_thinking_absent_when_capability_unknown();
   test_thinking_skips_anthropic_compatible_third_party();
   test_thinking_is_anthropic_only();
   test_thinking_disabled_is_unchanged();

   printf("all agent_request_build goldens passed\n");
   return 0;
}
