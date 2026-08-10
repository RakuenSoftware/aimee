#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "model_registry.h"

void test_agent_route_with_caps_honors_tools_enabled(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   cfg.agent_count = 2;
   strcpy(cfg.default_agent, "minimax");

   strcpy(cfg.agents[0].name, "minimax");
   strcpy(cfg.agents[0].provider, "minimax");
   strcpy(cfg.agents[0].model, "MiniMax-M2.7");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   strcpy(cfg.agents[0].api_key, "test-minimax-key");

   strcpy(cfg.agents[1].name, "no-tools");
   strcpy(cfg.agents[1].provider, "mistral");
   strcpy(cfg.agents[1].model, "mistral-large-latest");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;

   agent_t *routed = agent_route_with_caps(&cfg, "review", &sys_cfg, MODEL_CAP_TOOLS, 0);
   assert(routed == &cfg.agents[0]);

   cfg.agents[0].tools_enabled = 0;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, MODEL_CAP_TOOLS, 0) == NULL);
}

/* Routing must honor the per-agent context_window override so onboarding a
 * model the capability catalog doesn't know about is a config change, not a
 * code change to the registry table. */
void test_agent_route_with_caps_honors_context_override(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   /* Two agents so the GATE has a real choice to make. Asserting NULL is no
    * longer the way to observe rejection: an unsatisfiable min_context now
    * escalates to the largest seat rather than failing the request (a DEGREE
    * shortfall, best-effort). Rejection is therefore observed as "the other
    * agent was chosen". */
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "small-ctx");
   strcpy(cfg.agents[0].provider, "openai");
   /* gpt-4's catalog context window is 8192 — below the 50000 requirement. */
   strcpy(cfg.agents[0].model, "gpt-4");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].cost_tier = 0; /* cheapest, so only the gate can exclude it */
   strcpy(cfg.agents[0].api_key, "test-key");

   strcpy(cfg.agents[1].name, "big-ctx");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].middleware.context_window = 200000;
   strcpy(cfg.agents[1].api_key, "test-key");

   /* Catalog says 8192 < 50000, so the cheapest agent is dropped by the gate and
    * the larger one wins despite its higher tier. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[1]);

   /* An explicit per-agent override supersedes the catalog value, so the cheap
    * agent qualifies and cost-tier ordering puts it back in front — with no
    * change to the model registry table. */
   cfg.agents[0].middleware.context_window = 1000000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[0]);

   /* An override BELOW the requirement is still rejected by the gate. */
   cfg.agents[0].middleware.context_window = 1000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[1]);

   /* When NOTHING satisfies the requirement, routing escalates to the largest
    * seat rather than returning no route. */
   cfg.agents[1].middleware.context_window = 2000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[1]);
}

/* tools_enabled defaults from the backing model's intrinsic capability when the
 * JSON key is absent, so a tool-capable delegate is usable for tool-requiring
 * roles instead of being silently filtered out by the routing capability check
 * (delegate_filter_route_capabilities). Explicit values still win. Regression
 * for "no configured model supports required capabilities (caps=tools)". */
void test_tools_enabled_capability_default(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   /* m_default: tool-capable model (mistral), no tools_enabled key -> derive ON.
    * m_off:     same model, explicit "tools_enabled": false       -> stays OFF.
    * m_on:      same model, explicit "tools_enabled": true        -> stays ON. */
   fputs("{\"agents\":[{\"name\":\"m_default\",\"provider\":\"mistral\","
         "\"model\":\"mistral-medium-latest\",\"roles\":[\"review\"],"
         "\"backend\":\"provider-cli\",\"cli_kind\":\"mistral\",\"cli_cmd\":\"vibe\"},"
         "{\"name\":\"m_off\",\"provider\":\"mistral\","
         "\"model\":\"mistral-medium-latest\",\"roles\":[\"review\"],"
         "\"backend\":\"provider-cli\",\"cli_kind\":\"mistral\",\"cli_cmd\":\"vibe\","
         "\"tools_enabled\":false},"
         "{\"name\":\"m_on\",\"provider\":\"mistral\","
         "\"model\":\"mistral-medium-latest\",\"roles\":[\"review\"],"
         "\"backend\":\"provider-cli\",\"cli_kind\":\"mistral\",\"cli_cmd\":\"vibe\","
         "\"tools_enabled\":true}]}\n",
         f);
   fclose(f);

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   assert(loaded.agent_count == 3);

   const agent_t *m_default = agent_find(&loaded, "m_default");
   const agent_t *m_off = agent_find(&loaded, "m_off");
   const agent_t *m_on = agent_find(&loaded, "m_on");
   assert(m_default && m_off && m_on);

   /* The crux: an absent key on a tool-capable model now defaults tools ON. */
   assert(m_default->tools_enabled == 1);
   /* Explicit operator settings are still honoured verbatim. */
   assert(m_off->tools_enabled == 0);
   assert(m_on->tools_enabled == 1);

   unlink(agent_config_path());
   printf("  PASS: test_tools_enabled_capability_default\n");
}

/* A third-party vendor served over another vendor's WIRE FORMAT (MiniMax and
 * Moonshot/Kimi both expose Anthropic-compatible endpoints) must resolve its
 * capabilities under its own CATALOG identity. Before catalog_provider existed,
 * provider="anthropic" made every model_capability_get() miss and fall through to
 * the heuristic's anthropic branch, which matches no claude-* prefix: the models
 * lost MODEL_CAP_REASONING (and with it the long reasoning timeout) and resolved
 * a wrong or zero context window. */
void test_catalog_provider_separates_vendor_from_wire(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"MiniMax-M3\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.minimax.io/anthropic\",\"model\":\"MiniMax-M3\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]},"
         "{\"name\":\"kimi\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.kimi.com/coding/\",\"model\":\"kimi-k2.7-code\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]},"
         "{\"name\":\"opus\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.anthropic.com\",\"model\":\"claude-opus-4-8\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]}]}\n",
         f);
   fclose(f);

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   assert(loaded.agent_count == 3);

   const agent_t *mm = agent_find(&loaded, "MiniMax-M3");
   const agent_t *kimi = agent_find(&loaded, "kimi");
   const agent_t *opus = agent_find(&loaded, "opus");
   assert(mm && kimi && opus);

   /* The WIRE provider is untouched — it still drives the anthropic-version
    * header, the x-api-key auth coercion, and the credential env-var set. */
   assert(strcmp(mm->provider, "anthropic") == 0);
   assert(strcmp(kimi->provider, "anthropic") == 0);

   /* The CATALOG identity is the vendor, derived from the endpoint host. */
   assert(strcmp(agent_catalog_provider(mm), "minimax") == 0);
   assert(strcmp(agent_catalog_provider(kimi), "moonshotai") == 0);
   /* A genuine Anthropic endpoint keeps provider as its catalog identity. */
   assert(strcmp(agent_catalog_provider(opus), "anthropic") == 0);

   /* The payoff: capability now resolves under the vendor identity. Both are
    * tool-using REASONING models, and REASONING is what selects the long
    * per-call timeout (agent_config.c) — under the old "anthropic" identity the
    * heuristic matched no claude-* prefix, dropped REASONING, and both ran on
    * the short default timeout, whose symptom is slow completions cut off and
    * retried as spurious read failures. These must hold with a COLD models.dev
    * cache, so they assert the heuristic floor, not the live catalog. */
   model_capability_t cap;
   assert(model_capability_get(agent_catalog_provider(mm), mm->model, &cap) != 0);
   assert((cap.flags & MODEL_CAP_REASONING) != 0);
   assert((cap.flags & MODEL_CAP_TOOLS) != 0);
   /* MiniMax-M3 is a 1M-context model; the stale bare "minimax" prefix used to
    * report 200000 for it. */
   assert(cap.context_window == 1000000);

   assert(model_capability_get(agent_catalog_provider(kimi), kimi->model, &cap) != 0);
   assert((cap.flags & MODEL_CAP_REASONING) != 0);
   assert((cap.flags & MODEL_CAP_TOOLS) != 0);
   assert(cap.context_window == 262144);

   /* The reasoning-derived timeout is the observable consequence: both agents
    * must now get the long timeout rather than the short default. */
   assert(mm->timeout_ms == AGENT_REASONING_TIMEOUT_MS);
   assert(kimi->timeout_ms == AGENT_REASONING_TIMEOUT_MS);

   printf("  PASS: test_catalog_provider_separates_vendor_from_wire\n");

   unlink(agent_config_path());
}

/* Catalog derivation must match HOST LABELS, not a substring of the whole URL.
 * Review found four concrete ways substring matching misfires; each is asserted
 * here. A wrong derivation is silent — it surfaces only as wrong capability
 * flags, timeout, and context filtering — so these are the guard rails. */
void test_catalog_provider_host_matching_is_label_anchored(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[\n"
         /* Uppercase host: DNS is case-insensitive, strstr was not. */
         "{\"name\":\"upper\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://API.KIMI.COM/coding/\",\"model\":\"some-model\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]},\n"
         /* Vendor domain as a PATH segment of an unrelated gateway. */
         "{\"name\":\"pathy\",\"provider\":\"openai\","
         "\"endpoint\":\"https://gateway.example/v1/api.kimi.com/relay\","
         "\"model\":\"house-model\",\"auth_type\":\"bearer\",\"api_key\":\"k\","
         "\"roles\":[\"review\"]},\n"
         /* Lookalike suffix: kimi.com is a PREFIX of the host, not its domain. */
         "{\"name\":\"lookalike\",\"provider\":\"openai\","
         "\"endpoint\":\"https://api.kimi.com.attacker.example/v1\","
         "\"model\":\"house-model\",\"auth_type\":\"bearer\",\"api_key\":\"k\","
         "\"roles\":[\"review\"]},\n"
         /* Userinfo must not be read as the host. */
         "{\"name\":\"userinfo\",\"provider\":\"openai\","
         "\"endpoint\":\"https://api.minimax.io@gateway.example/v1\","
         "\"model\":\"house-model\",\"auth_type\":\"bearer\",\"api_key\":\"k\","
         "\"roles\":[\"review\"]},\n"
         /* Legitimate subdomain and port still derive. */
         "{\"name\":\"sub\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.minimax.io:8443/anthropic\",\"model\":\"house-model\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]}\n"
         "]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);
   assert(c.agent_count == 5);

   assert(strcmp(agent_catalog_provider(agent_find(&c, "upper")), "moonshotai") == 0);
   /* A path segment must never select a vendor: falls back to wire provider. */
   assert(strcmp(agent_catalog_provider(agent_find(&c, "pathy")), "openai") == 0);
   assert(strcmp(agent_catalog_provider(agent_find(&c, "lookalike")), "openai") == 0);
   assert(strcmp(agent_catalog_provider(agent_find(&c, "userinfo")), "openai") == 0);
   assert(strcmp(agent_catalog_provider(agent_find(&c, "sub")), "minimax") == 0);

   printf("  PASS: test_catalog_provider_host_matching_is_label_anchored\n");
   unlink(agent_config_path());
}

/* Review-driven parser hardening. Each case previously misderived or was
 * rejected; a misderivation is silent and, in the legacy wire rewrite, changes
 * auth type and credential env-var selection. */
void test_catalog_provider_endpoint_parser_edges(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[\n"
         /* Scheme-less endpoint smuggling an authority through a PATH segment. */
         "{\"name\":\"pathscheme\",\"provider\":\"openai\","
         "\"endpoint\":\"gateway.example/relay://api.minimax.io/v1\","
         "\"model\":\"house\",\"auth_type\":\"bearer\",\"api_key\":\"k\","
         "\"roles\":[\"review\"]},\n"
         /* Scheme-relative URL must still resolve its host. */
         "{\"name\":\"schemerel\",\"provider\":\"anthropic\","
         "\"endpoint\":\"//api.kimi.com/v1\",\"model\":\"house\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]},\n"
         /* Bracketed IPv6 literal must not parse as \"[\". */
         "{\"name\":\"v6\",\"provider\":\"openai\","
         "\"endpoint\":\"https://[2001:db8::1]:8443/v1\",\"model\":\"house\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]},\n"
         /* Family prefix must be boundary-anchored: NOT MiniMax. */
         "{\"name\":\"lookalikemodel\",\"provider\":\"openai\","
         "\"endpoint\":\"https://gw.example/v1\",\"model\":\"minimaximum-production\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]}\n"
         "]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);

   /* Host is gateway.example, so no vendor: catalog falls back to the wire
    * provider AND the legacy wire rewrite must not have fired. */
   const agent_t *ps = agent_find(&c, "pathscheme");
   assert(strcmp(agent_catalog_provider(ps), "openai") == 0);
   assert(strcmp(ps->provider, "openai") == 0);

   assert(strcmp(agent_catalog_provider(agent_find(&c, "schemerel")), "moonshotai") == 0);

   /* An IPv6 literal names no vendor domain; it must not derive one. */
   assert(strcmp(agent_catalog_provider(agent_find(&c, "v6")), "openai") == 0);

   /* "minimaximum-production" is not the minimax family: neither the catalog
    * identity nor - critically - the wire provider may change. */
   const agent_t *lk = agent_find(&c, "lookalikemodel");
   assert(strcmp(agent_catalog_provider(lk), "openai") == 0);
   assert(strcmp(lk->provider, "openai") == 0);

   printf("  PASS: test_catalog_provider_endpoint_parser_edges\n");
   unlink(agent_config_path());
}

/* Gateways (OpenRouter and friends) do not carry a vendor host, so the vendor
 * has to come from a namespaced model id. Review found "moonshotai/kimi-k2.7-code"
 * missed while "minimax/MiniMax-M3" worked by accident. */
void test_catalog_provider_namespaced_model_ids(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[\n"
         "{\"name\":\"ns_moon\",\"provider\":\"openrouter\","
         "\"endpoint\":\"https://openrouter.ai/api/v1\","
         "\"model\":\"moonshotai/kimi-k2.7-code\",\"auth_type\":\"bearer\","
         "\"api_key\":\"k\",\"roles\":[\"review\"]},\n"
         "{\"name\":\"ns_mini\",\"provider\":\"openrouter\","
         "\"endpoint\":\"https://openrouter.ai/api/v1\","
         "\"model\":\"minimax/MiniMax-M3\",\"auth_type\":\"bearer\","
         "\"api_key\":\"k\",\"roles\":[\"review\"]},\n"
         "{\"name\":\"bare_kimi\",\"provider\":\"openai\","
         "\"endpoint\":\"https://gw.example/v1\",\"model\":\"kimi-k2.7-code\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]},\n"
         /* Undecidable alias: must NOT guess, must fall back to wire provider. */
         "{\"name\":\"alias\",\"provider\":\"openai\","
         "\"endpoint\":\"https://gw.example/v1\",\"model\":\"deployment-123\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]}\n"
         "]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);
   assert(strcmp(agent_catalog_provider(agent_find(&c, "ns_moon")), "moonshotai") == 0);
   assert(strcmp(agent_catalog_provider(agent_find(&c, "ns_mini")), "minimax") == 0);
   assert(strcmp(agent_catalog_provider(agent_find(&c, "bare_kimi")), "moonshotai") == 0);
   assert(strcmp(agent_catalog_provider(agent_find(&c, "alias")), "openai") == 0);

   printf("  PASS: test_catalog_provider_namespaced_model_ids\n");
   unlink(agent_config_path());
}

/* aimee names some providers after the CLI or product rather than the vendor:
 * the Claude OAuth/CLI seat is provider "claude", the Codex seat is "chatgpt".
 * models.dev keys those vendors "anthropic" and "openai". Unmapped, the PRIMARY
 * agent resolved no capability flags at all, a 200k context against a real 1M,
 * and an 8192 output ceiling against a real 128k. */
void test_catalog_provider_maps_cli_provider_names(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[\n"
         "{\"name\":\"claude\",\"provider\":\"claude\",\"endpoint\":\"\","
         "\"model\":\"claude-opus-4-8\",\"auth_type\":\"none\",\"roles\":[\"review\"]},\n"
         "{\"name\":\"codex\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"model\":\"gpt-5.6-sol\",\"auth_type\":\"codex-oauth\","
         "\"roles\":[\"review\"]}\n"
         "]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);
   const agent_t *cl = agent_find(&c, "claude");
   const agent_t *cx = agent_find(&c, "codex");
   assert(cl && cx);

   /* Wire provider untouched — it still selects auth and CLI behaviour. */
   assert(strcmp(cl->provider, "claude") == 0);
   assert(strcmp(cx->provider, "chatgpt") == 0);
   /* Catalog identity is the vendor. */
   assert(strcmp(agent_catalog_provider(cl), "anthropic") == 0);
   assert(strcmp(agent_catalog_provider(cx), "openai") == 0);

   /* Capability now resolves. The output ceiling is the sharpest symptom: an
    * unmapped provider fell back to the non-reasoning 8192 default. */
   model_capability_t cap;
   assert(model_capability_get(agent_catalog_provider(cl), cl->model, &cap) != 0);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_TOOLS);
   /* The catalog value, not the operator's policy ceiling: with a cold cache the
    * static prefix table answers 200000 for claude-opus-4; with the real catalog
    * it is 1000000. Both are legitimate SOURCES, so pin the property that
    * actually regressed instead — an unmapped provider yielded NO flags and the
    * non-reasoning 8192 output ceiling. */
   assert(cap.context_window >= 200000);
   assert(model_max_output(agent_catalog_provider(cl), cl->model) >= 32768);

   assert(model_capability_get(agent_catalog_provider(cx), cx->model, &cap) != 0);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_TOOLS);
   assert(model_max_output(agent_catalog_provider(cx), cx->model) >= 32768);

   printf("  PASS: test_catalog_provider_maps_cli_provider_names\n");
   unlink(agent_config_path());
}

/* The moonshotai heuristic must not hand REASONING to every unknown Moonshot
 * model: that would select the long per-call timeout and satisfy a REASONING
 * requirement for a model nobody has verified. Only the k2/k3 families it is
 * actually known for get it; everything else gets the tool-using floor. */
void test_moonshot_heuristic_scopes_reasoning_to_known_families(void)
{
   model_capability_t cap;

   assert(model_capability_get("moonshotai", "kimi-k2.7-code", &cap) != 0);
   assert((cap.flags & MODEL_CAP_REASONING) != 0);
   assert((cap.flags & MODEL_CAP_TOOLS) != 0);

   assert(model_capability_get("moonshotai", "kimi-k3", &cap) != 0);
   assert((cap.flags & MODEL_CAP_REASONING) != 0);

   /* Unknown/future Moonshot id: tools yes, reasoning must be EARNED. */
   assert(model_capability_get("moonshotai", "moonshot-v1-8k", &cap) != 0);
   assert((cap.flags & MODEL_CAP_TOOLS) != 0);
   assert((cap.flags & MODEL_CAP_REASONING) == 0);

   printf("  PASS: test_moonshot_heuristic_scopes_reasoning_to_known_families\n");
}

/* An explicit operator catalog_provider always wins over derivation, and only an
 * explicit value round-trips through save (a derived guess must not be frozen
 * into config where it would outlive the derivation rules). */
void test_catalog_provider_explicit_round_trip(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"pinned\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.minimax.io/anthropic\",\"model\":\"MiniMax-M3\","
         "\"catalog_provider\":\"minimax-cn\",\"auth_type\":\"bearer\",\"api_key\":\"k\","
         "\"roles\":[\"review\"]},"
         "{\"name\":\"derived\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.kimi.com/coding/\",\"model\":\"kimi-k2.7-code\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]}]}\n",
         f);
   fclose(f);

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   const agent_t *pinned = agent_find(&loaded, "pinned");
   const agent_t *derived = agent_find(&loaded, "derived");
   assert(pinned && derived);

   /* Explicit value beats the endpoint-host derivation. */
   assert(strcmp(agent_catalog_provider(pinned), "minimax-cn") == 0);
   assert(pinned->catalog_provider_explicit == 1);
   /* Derived value is present but not marked explicit. */
   assert(strcmp(agent_catalog_provider(derived), "moonshotai") == 0);
   assert(derived->catalog_provider_explicit == 0);

   assert(agent_save_config(&loaded) == 0);

   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   const agent_t *pinned2 = agent_find(&reloaded, "pinned");
   const agent_t *derived2 = agent_find(&reloaded, "derived");
   assert(pinned2 && derived2);
   /* The pin survives the round trip... */
   assert(strcmp(agent_catalog_provider(pinned2), "minimax-cn") == 0);
   assert(pinned2->catalog_provider_explicit == 1);
   /* ...and the derived one is re-derived, still not explicit. */
   assert(strcmp(agent_catalog_provider(derived2), "moonshotai") == 0);
   assert(derived2->catalog_provider_explicit == 0);

   printf("  PASS: test_catalog_provider_explicit_round_trip\n");

   unlink(agent_config_path());
}

/* An UNKNOWN context window (0) must NOT satisfy a positive min_context. The
 * old gate guarded with `effective_ctx > 0`, so a model whose window aimee could
 * not establish passed for arbitrarily large prompts — the failure looked like
 * success. Unproven capacity now disqualifies the agent from cheap selection. */
void test_unknown_context_window_does_not_pass_min_context(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   /* A known-good peer makes exclusion observable. Asserting NULL would no
    * longer work: an unknown window disqualifies the agent from CHEAP selection,
    * but with nothing else available routing escalates to it rather than failing
    * the request. Exclusion is therefore "the peer was chosen instead". */
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "unknown-ctx");
   strcpy(cfg.agents[0].provider, "anthropic");
   /* A model id no catalog or prefix table knows: context window resolves 0. */
   strcpy(cfg.agents[0].model, "totally-unknown-model-xyz");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 0; /* cheapest, so only the gate can exclude it */
   strcpy(cfg.agents[0].api_key, "test-key");

   strcpy(cfg.agents[1].name, "known-ctx");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].middleware.context_window = 200000;
   strcpy(cfg.agents[1].api_key, "test-key");

   /* Unknown window + a real requirement -> excluded, so the dearer known-good
    * peer is chosen (previously the unknown agent was silently ADMITTED). */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[1]);

   /* No context requirement at all -> the cheap agent is routable again. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 0) == &cfg.agents[0]);

   /* An operator override supplies the missing window and restores routing. */
   cfg.agents[0].middleware.context_window = 200000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[0]);

   printf("  PASS: test_unknown_context_window_does_not_pass_min_context\n");
}

/* The catalog must report the true context window for the live third-party
 * models. Regression for the stale bare "minimax" prefix silently reporting
 * 200000 for MiniMax-M3 (true 1000000, a 5x understatement) and for kimi having
 * no prefix entry at all (resolving 0, which then passed the fail-open gate). */
void test_context_window_table_covers_live_vendors(void)
{
   assert(model_context_window("MiniMax-M3") == 1000000);
   /* 196608, not the round 200000 the prefix table used to report: that figure
    * was itself the rounding this test was written to catch, just one model
    * further down the list. The catalogue publishes the real window. */
   assert(model_context_window("MiniMax-M2") == 196608);
   assert(model_context_window("kimi-k2.7-code") == 262144);
   /* The bare fallback still resolves the oldest known family, never a newer. */
   assert(model_context_window("minimax") == 200000);

   printf("  PASS: test_context_window_table_covers_live_vendors\n");
}

/* A PRIMARY (user-facing) turn must reach the configured default agent whatever
 * its cost_tier. The default is the operator's most-capable-seat choice, and a
 * user must never be handed a weaker model because a cheaper peer exists.
 * Previously the default was only returned from inside the min_tier-filtered
 * pass, so a premium default with ANY cheaper peer serving the same role was
 * silently skipped — the exact opposite of the intent. */
void test_primary_turn_reaches_default_above_min_tier(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.default_agent, "premium");

   strcpy(cfg.agents[0].name, "premium");
   strcpy(cfg.agents[0].provider, "anthropic");
   strcpy(cfg.agents[0].model, "claude-opus-4-8");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 1; /* dearer than the peer below */
   strcpy(cfg.agents[0].api_key, "k");

   strcpy(cfg.agents[1].name, "cheap");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-haiku-4-5");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 0;
   strcpy(cfg.agents[1].api_key, "k");

   /* Delegation (NOT a primary turn) still minimises cost. */
   agent_routing_set_primary_turn(0);
   assert(agent_route(&cfg, "review") == &cfg.agents[1]);

   /* A primary turn reaches the premium default despite its higher tier. */
   agent_routing_set_primary_turn(1);
   assert(agent_route(&cfg, "review") == &cfg.agents[0]);

   /* A disabled default must not be handed back; fall through to routing. */
   cfg.agents[0].enabled = 0;
   assert(agent_route(&cfg, "review") == &cfg.agents[1]);
   cfg.agents[0].enabled = 1;

   /* A default that does not serve the role is not a usable seat either.
    * Selection is declared-role only: an agent is routable for a role solely
    * when its `roles` list names that role (or `all`). */
   strcpy(cfg.agents[0].roles[0], "review");
   strcpy(cfg.agents[1].roles[0], "explain");
   assert(agent_route(&cfg, "explain") == &cfg.agents[1]);

   /* No exec-role fallback: with the default's roles naming something other than
    * `review`, and no peer declaring `review`, the role is unroutable — the
    * default is NOT waved through just because `review` was once a default exec
    * role. This is the behaviour the $random/gemma4 fix depends on. */
   strcpy(cfg.agents[0].roles[0], "explain");
   assert(agent_route(&cfg, "review") == NULL);

   /* Session affinity outranks the default: a tmux agent holds a STATEFUL
    * session, so a primary turn must not be yanked to an HTTP default and
    * abandon it. Only cost_tier is bypassed, never the tmux preference.
    *
    * A tmux agent is only ROUTABLE when tmux is actually installed
    * (agent_routing_block_reason returns MISSING_COMMAND otherwise), so this
    * assertion is environment-dependent and is skipped rather than weakened
    * into something that passes for the wrong reason. */
   if (access("/usr/bin/tmux", X_OK) == 0 || access("/bin/tmux", X_OK) == 0)
   {
      strcpy(cfg.agents[0].roles[0], "review");
      strcpy(cfg.agents[1].roles[0], "review");
      strcpy(cfg.agents[1].backend, AGENT_BACKEND_TMUX_CLI);
      strcpy(cfg.agents[1].cli_cmd, "sh"); /* on PATH so only tmux gates it */
      assert(agent_route(&cfg, "review") == &cfg.agents[1]);

      /* With no tmux peer the default wins again. */
      cfg.agents[1].backend[0] = '\0';
      cfg.agents[1].cli_cmd[0] = '\0';
      assert(agent_route(&cfg, "review") == &cfg.agents[0]);
   }
   else
   {
      printf("  SKIP: tmux session-affinity assertion (tmux not installed)\n");
   }

   agent_routing_set_primary_turn(0);
   printf("  PASS: test_primary_turn_reaches_default_above_min_tier\n");
}

/* Under capability routing the primary default still wins on price, but it must
 * genuinely SATISFY the requirements — a seat that cannot hold the prompt is not
 * usable just because it is the default. */
void test_primary_turn_default_must_still_satisfy_caps(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   cfg.agent_count = 2;
   strcpy(cfg.default_agent, "small_default");

   strcpy(cfg.agents[0].name, "small_default");
   strcpy(cfg.agents[0].provider, "openai");
   strcpy(cfg.agents[0].model, "gpt-4"); /* 8192 catalog window */
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 1;
   strcpy(cfg.agents[0].api_key, "k");

   strcpy(cfg.agents[1].name, "big_peer");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 0;
   cfg.agents[1].middleware.context_window = 200000;
   strcpy(cfg.agents[1].api_key, "k");

   agent_routing_set_primary_turn(1);
   /* No requirement: the default wins regardless of tier. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 0) == &cfg.agents[0]);
   /* A 50k prompt does not fit the default -> it is skipped, not forced. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[1]);
   agent_routing_set_primary_turn(0);

   printf("  PASS: test_primary_turn_default_must_still_satisfy_caps\n");
}

/* The REQUEST's output limit must be clamped to the context window, not merely
 * the prompt-budget arithmetic. agent_exec_context_budget_chars() clamps its own
 * local maths, but that cannot constrain what the provider is asked to emit — a
 * 200k-window agent pinned at 300k still asked for 300k of output until this
 * clamp existed. */
void test_request_max_tokens_clamped_to_context_window(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   strcpy(ag.provider, "anthropic");
   strcpy(ag.model, "claude-opus-4-8");
   ag.middleware.context_window = 200000;

   /* A pinned agent cap larger than the window is a misconfiguration. */
   ag.max_tokens = 300000;
   assert(agent_request_max_tokens(&ag, 0) == 100000);

   /* Equal to the window leaves no prompt room either. */
   ag.max_tokens = 200000;
   assert(agent_request_max_tokens(&ag, 0) == 100000);

   /* A sane pinned cap passes through untouched. */
   ag.max_tokens = 8192;
   assert(agent_request_max_tokens(&ag, 0) == 8192);

   /* A caller-supplied budget is clamped on the same rule. */
   ag.max_tokens = 0;
   assert(agent_request_max_tokens(&ag, 500000) == 100000);
   assert(agent_request_max_tokens(&ag, 4096) == 4096);

   /* With no operator override the CATALOG window is the ceiling. Consulting
    * only middleware.context_window left a catalogued small-window model
    * accepting an oversized pinned cap.
    *
    * Derive the over-pin from the resolved window rather than hardcoding one:
    * this previously pinned 300000 against claude-opus-4-8 on the assumption it
    * resolved to ~200000, which held only while the bundled snapshot was
    * unreachable and the heuristic answered. The real catalog gives it a far
    * larger window, so 300000 stopped being an over-pin and the clamp went
    * untested. */
   ag.middleware.context_window = 0;
   {
      model_capability_t cap;
      assert(model_capability_get(agent_catalog_provider(&ag), ag.model, &cap) != 0);
      assert(cap.context_window > 0);
      ag.max_tokens = cap.context_window; /* unambiguously above half the window */
      assert(agent_request_max_tokens(&ag, 0) == cap.context_window / 2);
      /* A pin BELOW the ceiling is honoured rather than raised. */
      ag.max_tokens = cap.context_window / 4;
      assert(agent_request_max_tokens(&ag, 0) == cap.context_window / 4);
   }

   /* A model with NO known window anywhere has nothing to clamp against. */
   memset(&ag, 0, sizeof(ag));
   strcpy(ag.provider, "anthropic");
   strcpy(ag.model, "totally-unknown-model-qqq");
   ag.max_tokens = 300000;
   assert(agent_request_max_tokens(&ag, 0) == 300000);

   printf("  PASS: test_request_max_tokens_clamped_to_context_window\n");
}

/* FAIL UPWARD: when capability filtering eliminates every candidate, routing
 * escalates to the most capable seat instead of reporting "no route".
 *
 * Two operator invariants collide when nothing qualifies — never fail a request,
 * and never send a packet to a model that cannot complete it. Resolving toward
 * the BEST seat makes the cost of being wrong "we overspent" (visible,
 * recoverable) rather than an outage or a garbage answer. Crucially it must NOT
 * fall back to the cheapest agent, which is exactly the seat the gate rejected. */
void test_capability_gate_escalates_instead_of_failing(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   cfg.agent_count = 2;

   /* Cheapest, smallest window: the seat the gate rejects. */
   strcpy(cfg.agents[0].name, "small_cheap");
   strcpy(cfg.agents[0].provider, "openai");
   strcpy(cfg.agents[0].model, "gpt-4"); /* 8192 catalog window */
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 0;
   strcpy(cfg.agents[0].api_key, "k");

   /* Larger window, dearer tier — still short of the requirement below. */
   strcpy(cfg.agents[1].name, "big_dear");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 3;
   cfg.agents[1].middleware.context_window = 200000;
   strcpy(cfg.agents[1].api_key, "k");

   /* A requirement NO agent satisfies. Previously this returned NULL, i.e. the
    * request failed outright. It must now escalate to the largest window. */
   agent_t *routed = agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000);
   assert(routed != NULL);
   assert(routed == &cfg.agents[1]);
   /* Emphatically NOT the cheapest seat the gate just rejected. */
   assert(routed != &cfg.agents[0]);

   /* The configured default is the operator's most-capable choice and wins the
    * escalation even when a peer has a larger window. */
   strcpy(cfg.default_agent, "small_cheap");
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == &cfg.agents[0]);
   cfg.default_agent[0] = '\0';

   /* A satisfiable requirement must NOT escalate — normal routing still wins,
    * and still picks the cheapest qualifying seat. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 100000) == &cfg.agents[1]);

   /* Genuinely nothing enabled is a real outage, not a filtering artifact: NULL. */
   cfg.agents[0].enabled = 0;
   cfg.agents[1].enabled = 0;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == NULL);
   cfg.agents[0].enabled = 1;
   cfg.agents[1].enabled = 1;

   /* A KIND shortfall must NOT escalate. Escalation is a best effort at a DEGREE
    * problem (prompt bigger than any window); it cannot conjure a capability
    * nobody has, and dispatching anyway would trade a clear config error for a
    * doomed request. Tools required, none offered -> still "no route". */
   cfg.agents[0].tools_enabled = 0;
   cfg.agents[1].tools_enabled = 0;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, MODEL_CAP_TOOLS, 0) == NULL);
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, MODEL_CAP_TOOLS, 5000000) == NULL);
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[1].tools_enabled = 1;

   printf("  PASS: test_capability_gate_escalates_instead_of_failing\n");
}

/* Escalation must not re-admit an agent the ordinary pass excluded. Review raised
 * this as a High — that escalation reconstructs eligibility and could bypass a
 * gate. It cannot: every gate lives inside agent_is_available_for_routing(),
 * which escalation calls.
 *
 * Covered here: the generic POLICY filter callback (the mechanism by which the
 * server enforces primary_only — the flag itself is inert without that filter
 * registered, so a unit test can only exercise the callback), the HEALTH filter
 * callback, and the Claude-CLI STRUCTURAL rule, which agent_config enforces
 * directly and so is testable as itself. */
static int esc_policy_excludes_all(const agent_t *ag)
{
   (void)ag;
   return 1; /* exclude every agent */
}

static int esc_health_down_all(const char *name)
{
   (void)name;
   return 1; /* every agent DOWN */
}

void test_escalation_respects_policy_and_health_gates(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   cfg.agent_count = 1;
   strcpy(cfg.default_agent, "only"); /* also exercises the fast path */
   strcpy(cfg.agents[0].name, "only");
   strcpy(cfg.agents[0].provider, "openai");
   strcpy(cfg.agents[0].model, "gpt-4"); /* 8192 window, far below the ask */
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   strcpy(cfg.agents[0].api_key, "k");

   /* Baseline: an impossible context requirement escalates to this agent. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == &cfg.agents[0]);

   /* POLICY excluded (this is how primary_only is enforced) -> no route, not an
    * escalation that ignores the policy. */
   agent_set_route_policy_filter(esc_policy_excludes_all);
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == NULL);
   agent_set_route_policy_filter(NULL);

   /* HEALTH down -> likewise no route. */
   agent_set_route_health_filter(esc_health_down_all);
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == NULL);
   agent_set_route_health_filter(NULL);

   /* Gates cleared: escalation works again, proving the NULLs above were the
    * gates and not some unrelated failure. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == &cfg.agents[0]);

   /* The Claude-CLI STRUCTURAL gate: a claude CLI seat that is not server-hosted
    * has no session to drive, so it is excluded regardless of any filter.
    * Escalation must honour that too. */
   strcpy(cfg.agents[0].backend, AGENT_BACKEND_TMUX_CLI);
   strcpy(cfg.agents[0].cli_kind, "claude");
   cfg.agents[0].is_server_hosted = 0;
   assert(agent_is_available_for_routing(&cfg.agents[0]) == 0);
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == NULL);
   cfg.agents[0].backend[0] = '\0';
   cfg.agents[0].cli_kind[0] = '\0';

   printf("  PASS: test_escalation_respects_policy_and_health_gates\n");
}

/* Escalation is gated on capability routing: with the flag OFF, plain cost-tier
 * routing must be byte-identical to before. */
void test_no_escalation_when_capability_routing_disabled(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 0; /* default */

   cfg.agent_count = 1;
   strcpy(cfg.agents[0].name, "only");
   strcpy(cfg.agents[0].provider, "openai");
   strcpy(cfg.agents[0].model, "gpt-4");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   strcpy(cfg.agents[0].api_key, "k");

   /* Flag off -> agent_route(), which ignores caps entirely. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 5000000) == &cfg.agents[0]);

   printf("  PASS: test_no_escalation_when_capability_routing_disabled\n");
}

/* Slice-1 evidence: does enabling model_meta_capability_routing change routing
 * for a fleet shaped like the live one? Four agents, three at tier 0 all
 * declaring roles ["all"], one premium default at tier 1 — the configuration
 * that made the premium seat unreachable in the first place.
 *
 * This is a BEHAVIOUR-DIFF test, not an aspiration: it pins what the flag
 * actually does today so the flip is an informed decision rather than a hope. */
void test_capability_routing_flag_behaviour_diff(void)
{
   agent_config_t cfg;
   agent_route_policy_t off, on;
   memset(&cfg, 0, sizeof(cfg));
   memset(&off, 0, sizeof(off));
   memset(&on, 0, sizeof(on));
   on.capability_routing = 1;

   cfg.agent_count = 4;
   strcpy(cfg.default_agent, "claude");

   /* Premium default, tier 1, 200k policy ceiling. */
   strcpy(cfg.agents[0].name, "claude");
   strcpy(cfg.agents[0].provider, "claude"); /* CLI provider name */
   strcpy(cfg.agents[0].model, "claude-opus-4-8");
   strcpy(cfg.agents[0].roles[0], "all");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 1;
   cfg.agents[0].middleware.context_window = 200000;
   strcpy(cfg.agents[0].api_key, "k");

   /* Three tier-0 peers, all claiming every role. */
   struct
   {
      const char *name, *provider, *model, *endpoint;
      int ctx;
   } cheap[] = {
       {"codex", "chatgpt", "gpt-5.6-sol", "https://chatgpt.com/backend-api/codex", 272000},
       {"MiniMax-M3", "anthropic", "MiniMax-M3", "https://api.minimax.io/anthropic", 0},
       {"kimi", "anthropic", "kimi-k2.7-code", "https://api.kimi.com/coding/", 0},
   };
   for (int i = 0; i < 3; i++)
   {
      agent_t *ag = &cfg.agents[i + 1];
      strcpy(ag->name, cheap[i].name);
      strcpy(ag->provider, cheap[i].provider);
      strcpy(ag->model, cheap[i].model);
      strcpy(ag->endpoint, cheap[i].endpoint);
      strcpy(ag->roles[0], "all");
      ag->role_count = 1;
      ag->enabled = 1;
      ag->tools_enabled = 1;
      ag->cost_tier = 0;
      ag->middleware.context_window = cheap[i].ctx;
      strcpy(ag->api_key, "k");
   }

   /* A SMALL packet: every agent qualifies, so both modes pick a tier-0 peer.
    * The premium default stays unreachable for delegation either way — that is
    * a cost_tier problem, not something the capability flag fixes. */
   agent_t *r_off = agent_route_with_caps(&cfg, "review", &off, 0, 0);
   agent_t *r_on = agent_route_with_caps(&cfg, "review", &on, 0, 0);
   assert(r_off && r_on);
   assert(r_off->cost_tier == 0 && r_on->cost_tier == 0);

   /* A LARGE packet (300k). With the flag OFF, capability is never consulted, so
    * routing can hand it to an agent whose window cannot hold it. With the flag
    * ON, only agents that can actually hold it survive. This is the flag's whole
    * value, and the reason the catalog-identity work had to land first: without
    * it MiniMax/kimi resolved a wrong or zero window. */
   /* Sample the rotation: agent_pick_balanced() round-robins tier peers on a
    * process-wide cursor, so a SINGLE call proves nothing about which agent each
    * mode prefers — it would pass even if both modes returned the same seat. */
   int off_picked_incapable = 0, on_picked_incapable = 0;
   for (int i = 0; i < 9; i++)
   {
      agent_t *a_off = agent_route_with_caps(&cfg, "review", &off, 0, 300000);
      agent_t *a_on = agent_route_with_caps(&cfg, "review", &on, 0, 300000);
      assert(a_off && a_on);

      /* MiniMax-M3 (1M catalog window) is the only seat here that holds 300k;
       * codex is capped at 272k by policy and kimi at 262144 by catalog. */
      if (strcmp(a_off->name, "MiniMax-M3") != 0)
         off_picked_incapable = 1;
      if (strcmp(a_on->name, "MiniMax-M3") != 0)
         on_picked_incapable = 1;
      /* ON must ALWAYS choose the capable seat, on every rotation. */
      assert(strcmp(a_on->name, "MiniMax-M3") == 0);
   }
   /* OFF ignores capability, so across a full rotation it demonstrably hands the
    * 300k packet to a seat that cannot hold it. That difference is the flag. */
   assert(off_picked_incapable);
   assert(!on_picked_incapable);

   printf("  PASS: test_capability_routing_flag_behaviour_diff\n");
}

/* Provider-general registration: one operator entry, one runtime target per
 * model. The operator writes "codex" once with a models list; routing, health,
 * admission and fallback continue to work per-model because each target IS an
 * agent — no routing signature changes.
 *
 * Tiers must be DERIVED per model: sol/terra/luna are $5.00/$2.50/$1.00, so a
 * single inherited tier would make "cheapest first" pick arbitrarily among them. */
/* Seed a priced catalog under the CURRENT HOME. Tier derivation reads the model
 * catalog, and without it every expanded target keeps the registration's
 * declared tier — the assertions below would then pass or fail for reasons
 * unrelated to derivation. */
static void seed_codex_prices(void)
{
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return;
   char dir[512], path[600];
   snprintf(dir, sizeof(dir), "%s/.cache", home);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/.cache/aimee", home);
   mkdir(dir, 0755);
   snprintf(path, sizeof(path), "%s/models_dev.json", dir);
   FILE *cf = fopen(path, "w");
   if (!cf)
      return;
   fputs("{\"openai\":{\"models\":{"
         "\"gpt-5.6-sol\":{\"limit\":{\"context\":1050000},"
         "  \"cost\":{\"input\":5.0,\"output\":30.0},\"tool_call\":true},"
         "\"gpt-5.6-terra\":{\"limit\":{\"context\":1050000},"
         "  \"cost\":{\"input\":2.5,\"output\":15.0},\"tool_call\":true},"
         "\"gpt-5.6-luna\":{\"limit\":{\"context\":1050000},"
         "  \"cost\":{\"input\":1.0,\"output\":6.0},\"tool_call\":true}"
         "}}}",
         cf);
   fclose(cf);
}

void test_provider_general_registration_expands(void)
{
   seed_codex_prices();
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":["
         "{\"name\":\"codex\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"cost_tier\":0,\"roles\":[\"review\"],"
         "\"models\":[\"gpt-5.6-sol\",\"gpt-5.6-terra\",\"gpt-5.6-luna\"]},"
         /* A LEGACY single-model entry must keep exactly its old meaning. */
         "{\"name\":\"legacy\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.anthropic.com\",\"model\":\"claude-opus-4-8\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"cost_tier\":1,"
         "\"roles\":[\"review\"]}"
         "]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);
   /* Three expanded targets plus the untouched legacy entry. The registration
    * itself is NOT a route target. */
   assert(c.agent_count == 4);
   assert(agent_find(&c, "codex") == NULL);

   const agent_t *sol = agent_find(&c, "codex:gpt-5.6-sol");
   const agent_t *terra = agent_find(&c, "codex:gpt-5.6-terra");
   const agent_t *luna = agent_find(&c, "codex:gpt-5.6-luna");
   assert(sol && terra && luna);

   /* Registration properties are inherited... */
   assert(strcmp(sol->provider, "chatgpt") == 0);
   assert(strcmp(sol->auth_type, "codex-oauth") == 0);
   assert(sol->enabled == 1);
   /* ...and the catalog identity is derived, so capability/price resolve. */
   assert(strcmp(agent_catalog_provider(sol), "openai") == 0);

   /* Tiers are DERIVED per model from price, not inherited from the
    * registration's declared 0: sol $5 > terra $2.50 > luna $1.00. */
   assert(sol->cost_tier > terra->cost_tier);
   assert(terra->cost_tier > luna->cost_tier);

   /* The legacy entry is untouched: same name, same model, declared tier kept. */
   const agent_t *legacy = agent_find(&c, "legacy");
   assert(legacy && strcmp(legacy->model, "claude-opus-4-8") == 0);
   assert(legacy->cost_tier == 1);

   printf("  PASS: test_provider_general_registration_expands\n");
   unlink(agent_config_path());
}

/* "models": "auto" is the operator experience the requirement actually asks for:
 * register the provider, name no models. It resolves the provider profile's
 * CURATED allowlist rather than the raw catalog — a model appearing in a
 * provider's listing does not prove it is intended for this product, has
 * complete capability metadata, or is enabled for the account. */
void test_provider_general_auto_uses_curated_allowlist(void)
{
   seed_codex_prices();
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":["
         "{\"name\":\"codex\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
         "\"models\":\"auto\"}"
         "]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);
   /* The openai profile curates exactly sol/terra/luna. */
   assert(c.agent_count == 3);
   assert(agent_find(&c, "codex:gpt-5.6-sol") != NULL);
   assert(agent_find(&c, "codex:gpt-5.6-terra") != NULL);
   assert(agent_find(&c, "codex:gpt-5.6-luna") != NULL);
   /* Registration itself is not routable. */
   assert(agent_find(&c, "codex") == NULL);
   /* Tiers still derive per model. */
   assert(agent_find(&c, "codex:gpt-5.6-sol")->cost_tier >
          agent_find(&c, "codex:gpt-5.6-luna")->cost_tier);

   printf("  PASS: test_provider_general_auto_uses_curated_allowlist\n");
   unlink(agent_config_path());
}

/* "auto" against a provider with no curated set must FAIL rather than expose
 * whatever the provider happens to list, and an unrecognised string is not a
 * silent no-op. */
void test_provider_general_auto_requires_curated_set(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"x\",\"provider\":\"ollama\","
         "\"endpoint\":\"http://localhost:11434\",\"auth_type\":\"none\","
         "\"roles\":[\"review\"],\"models\":\"auto\"}]}\n",
         f);
   fclose(f);
   agent_config_t c;
   assert(agent_load_config(&c) != 0);

   f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"x\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
         "\"models\":\"everything\"}]}\n",
         f);
   fclose(f);
   assert(agent_load_config(&c) != 0);

   printf("  PASS: test_provider_general_auto_requires_curated_set\n");
   unlink(agent_config_path());
}

/* A duplicate model id must REJECT the config. Two targets would share a name,
 * and both health and --via key on it; silently registering fewer models than
 * the operator declared would route work to a set they never approved. Also
 * covers the other ill-formed shapes: an empty models array, a non-string
 * entry, a name colliding with a legacy agent, and `model` alongside `models`. */
void test_provider_general_rejects_malformed_registrations(void)
{
   seed_codex_prices();
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"codex\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
         "\"models\":[\"gpt-5.6-sol\",\"gpt-5.6-sol\",\"gpt-5.6-luna\"]}]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) != 0); /* duplicate model id */

   /* Empty models array. */
   f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"c\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],\"models\":[]}]}\n",
         f);
   fclose(f);
   assert(agent_load_config(&c) != 0);

   /* Non-string entry. */
   f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"c\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
         "\"models\":[\"gpt-5.6-sol\",7]}]}\n",
         f);
   fclose(f);
   assert(agent_load_config(&c) != 0);

   /* `model` AND `models`: the single value would be silently discarded. */
   f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"c\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
         "\"model\":\"gpt-5.6-sol\",\"models\":[\"gpt-5.6-luna\"]}]}\n",
         f);
   fclose(f);
   assert(agent_load_config(&c) != 0);

   /* A LEGACY agent already holding the generated name must collide, not be
    * shadowed by an unreachable duplicate — in EITHER declaration order. A scan
    * of only the agents committed so far catches one ordering and misses the
    * other, which is why the check is a whole-config pass. */
   const char *legacy_first =
       "{\"agents\":["
       "{\"name\":\"codex:gpt-5.6-sol\",\"provider\":\"openai\","
       "\"endpoint\":\"https://api.openai.com/v1\",\"model\":\"gpt-5.6-sol\","
       "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]},"
       "{\"name\":\"codex\",\"provider\":\"chatgpt\","
       "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
       "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
       "\"models\":[\"gpt-5.6-sol\"]}]}\n";
   const char *expansion_first =
       "{\"agents\":["
       "{\"name\":\"codex\",\"provider\":\"chatgpt\","
       "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
       "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
       "\"models\":[\"gpt-5.6-sol\"]},"
       "{\"name\":\"codex:gpt-5.6-sol\",\"provider\":\"openai\","
       "\"endpoint\":\"https://api.openai.com/v1\",\"model\":\"gpt-5.6-sol\","
       "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]}]}\n";
   for (int order = 0; order < 2; order++)
   {
      f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs(order == 0 ? legacy_first : expansion_first, f);
      fclose(f);
      assert(agent_load_config(&c) != 0);
   }

   printf("  PASS: test_provider_general_rejects_malformed_registrations\n");
   unlink(agent_config_path());
}

/* An operator who pins catalog_provider on a provider-general registration - a
 * gateway speaking one wire format while serving another vendor's models - means
 * it for every generated target. Discarding it would silently swap the vendor
 * identity that drives capability lookup, price, tier derivation and the
 * canonical model ref. */
void test_provider_general_preserves_explicit_catalog_provider(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"gw\",\"provider\":\"openai\","
         "\"endpoint\":\"https://gateway.example/v1\",\"auth_type\":\"bearer\","
         "\"api_key\":\"k\",\"catalog_provider\":\"anthropic\","
         "\"roles\":[\"review\"],"
         "\"models\":[\"claude-opus-4-8\",\"claude-haiku-4-5\"]}]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);
   assert(c.agent_count == 2);

   const agent_t *opus = agent_find(&c, "gw:claude-opus-4-8");
   const agent_t *haiku = agent_find(&c, "gw:claude-haiku-4-5");
   assert(opus && haiku);
   /* The pin survives on EVERY target, and is still marked explicit. */
   assert(strcmp(agent_catalog_provider(opus), "anthropic") == 0);
   assert(strcmp(agent_catalog_provider(haiku), "anthropic") == 0);
   assert(opus->catalog_provider_explicit == 1);
   /* Wire provider untouched. */
   assert(strcmp(opus->provider, "openai") == 0);

   printf("  PASS: test_provider_general_preserves_explicit_catalog_provider\n");
   unlink(agent_config_path());
}

/* An expansion that does not fit must reject the WHOLE config rather than
 * register a partial fleet: silently dropping models an operator declared would
 * route work to a set they never approved. */
void test_provider_general_overflow_rejects_config(void)
{
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":[{\"name\":\"big\",\"provider\":\"openai\","
         "\"endpoint\":\"https://api.openai.com/v1\",\"auth_type\":\"bearer\","
         "\"api_key\":\"k\",\"roles\":[\"review\"],\"models\":["
         "\"m1\",\"m2\",\"m3\",\"m4\",\"m5\",\"m6\",\"m7\",\"m8\","
         "\"m9\",\"m10\",\"m11\",\"m12\",\"m13\",\"m14\",\"m15\",\"m16\","
         "\"m17\"]}]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) != 0);

   printf("  PASS: test_provider_general_overflow_rejects_config\n");
   unlink(agent_config_path());
}

/* Fallback prefers a peer from the SAME provider registration. Grouping is by the
 * STORED registration, set at expansion, NOT by parsing the name: a legacy agent
 * coincidentally named "gw:backup" must not be treated as a sibling of targets
 * generated by a registration "gw", and a registration named "gw:east" must not
 * flatten to "gw". Both would group seats with unrelated endpoints and
 * credentials, which is what the preference exists to avoid. */
void test_registration_grouping(void)
{
   seed_codex_prices();
   FILE *f = fopen(agent_config_path(), "w");
   assert(f != NULL);
   fputs("{\"agents\":["
         "{\"name\":\"codex\",\"provider\":\"chatgpt\","
         "\"endpoint\":\"https://chatgpt.com/backend-api/codex\","
         "\"auth_type\":\"codex-oauth\",\"roles\":[\"review\"],"
         "\"models\":[\"gpt-5.6-sol\",\"gpt-5.6-luna\"]},"
         /* A LEGACY agent whose name merely LOOKS like a codex target. */
         "{\"name\":\"codex:legacy\",\"provider\":\"anthropic\","
         "\"endpoint\":\"https://api.anthropic.com\",\"model\":\"claude-opus-4-8\","
         "\"auth_type\":\"bearer\",\"api_key\":\"k\",\"roles\":[\"review\"]}"
         "]}\n",
         f);
   fclose(f);

   agent_config_t c;
   assert(agent_load_config(&c) == 0);

   const agent_t *sol = agent_find(&c, "codex:gpt-5.6-sol");
   const agent_t *luna = agent_find(&c, "codex:gpt-5.6-luna");
   const agent_t *legacy = agent_find(&c, "codex:legacy");
   assert(sol && luna && legacy);

   /* Generated targets carry the registration that produced them. */
   assert(strcmp(sol->registration, "codex") == 0);
   assert(strcmp(luna->registration, "codex") == 0);
   /* The legacy agent was NOT generated, so it belongs to no registration and is
    * therefore not a sibling - even though a name-prefix parse would say it is. */
   assert(legacy->registration[0] == '\0');

   /* ROUND TRIP. agent_save_config writes cfg->agents, and expansion has already
    * replaced the registration with its generated targets - so a save collapses
    * the operator's `models` form into individual agents. Pin what actually
    * happens so the behaviour is a decision rather than a surprise. */
   assert(agent_save_config(&c) == 0);
   agent_config_t rt;
   assert(agent_load_config(&rt) == 0);
   assert(agent_find(&rt, "codex:gpt-5.6-sol") != NULL);
   assert(agent_find(&rt, "codex:gpt-5.6-luna") != NULL);
   {
      /* `registration` must SURVIVE the save. It is set during expansion, and the
       * expanded targets are what get written, so without persisting it the
       * same-registration fallback preference silently disappeared the first
       * time anything saved the config - verified empirically before the fix. */
      const agent_t *sol_rt = agent_find(&rt, "codex:gpt-5.6-sol");
      const agent_t *luna_rt = agent_find(&rt, "codex:gpt-5.6-luna");
      assert(strcmp(sol_rt->registration, "codex") == 0);
      assert(strcmp(luna_rt->registration, "codex") == 0);
      const agent_t *legacy_rt = agent_find(&rt, "codex:legacy");
      assert(legacy_rt && legacy_rt->registration[0] == '\0');
   }

   {
      /* The persisted field is only worth persisting because fallback GROUPS on
       * it. Pin the grouping rule itself, not just that the string survived: a
       * change that kept the field but stopped grouping on it would otherwise
       * pass. */
      agent_t *sol_rt = agent_find(&rt, "codex:gpt-5.6-sol");
      agent_t *luna_rt = agent_find(&rt, "codex:gpt-5.6-luna");
      agent_t *legacy_rt = agent_find(&rt, "codex:legacy");
      assert(agent_same_registration(sol_rt, luna_rt) == 1);
      assert(agent_same_registration(sol_rt, sol_rt) == 1);
      /* The legacy agent's NAME shares the "codex" prefix, so a prefix parse
       * would call it a sibling of sol and hand fallback a seat with unrelated
       * endpoint and credentials. It has no registration, so it is its own. */
      assert(agent_same_registration(sol_rt, legacy_rt) == 0);
      assert(agent_same_registration(legacy_rt, sol_rt) == 0);
      /* Two UNREGISTERED agents are not siblings of each other either — empty
       * must not compare equal to empty. */
      agent_t bare_a, bare_b;
      memset(&bare_a, 0, sizeof(bare_a));
      memset(&bare_b, 0, sizeof(bare_b));
      assert(agent_same_registration(&bare_a, &bare_b) == 0);
      /* A registration whose own name contains ':' must not be flattened to the
       * text before it: "gw:east" and "gw:west" are distinct registrations. */
      agent_t east, west;
      memset(&east, 0, sizeof(east));
      memset(&west, 0, sizeof(west));
      snprintf(east.registration, sizeof(east.registration), "gw:east");
      snprintf(west.registration, sizeof(west.registration), "gw:west");
      assert(agent_same_registration(&east, &west) == 0);
      snprintf(west.registration, sizeof(west.registration), "gw:east");
      assert(agent_same_registration(&east, &west) == 1);
      assert(agent_same_registration(NULL, sol_rt) == 0);
      assert(agent_same_registration(sol_rt, NULL) == 0);
   }

   printf("  PASS: test_registration_grouping\n");
   unlink(agent_config_path());
}

/* The prefix helper remains for display/diagnostic use. */
void test_registration_prefix(void)
{
   char buf[MAX_AGENT_NAME];

   agent_registration_prefix("codex:gpt-5.6-sol", buf, sizeof(buf));
   assert(strcmp(buf, "codex") == 0);

   /* A legacy agent has no ':' and is its own registration, so it simply has no
    * siblings — the same-registration pass finds nothing and costs it nothing. */
   agent_registration_prefix("claude", buf, sizeof(buf));
   assert(strcmp(buf, "claude") == 0);

   /* Only the FIRST ':' separates; a model id containing one stays intact. */
   agent_registration_prefix("gw:vendor:model", buf, sizeof(buf));
   assert(strcmp(buf, "gw") == 0);

   /* Degenerate inputs must not read past the buffer or invent a prefix. */
   agent_registration_prefix(":leading", buf, sizeof(buf));
   assert(buf[0] == '\0');
   agent_registration_prefix("", buf, sizeof(buf));
   assert(buf[0] == '\0');
   agent_registration_prefix(NULL, buf, sizeof(buf));
   assert(buf[0] == '\0');

   /* Truncation is bounded by the output buffer. */
   char small[4];
   agent_registration_prefix("abcdefgh:model", small, sizeof(small));
   assert(strlen(small) == 3);

   printf("  PASS: test_registration_prefix\n");
}

/* Fine-grained role routing WORKS TODAY; it is the DEFAULT that is permissive.
 *
 * An earlier note in the routing proposal claimed role filtering was "bypassed
 * twice" and that a role split therefore needed a code change first. That is
 * wrong, and this test is the evidence. Two declarations control it:
 *
 *   - `roles` must not contain the "all" wildcard, which matches every role;
 *   - `exec_roles`, once non-empty, makes exec-role eligibility EXACT — the
 *     18-role default set applies only while an agent declares none.
 *
 * So splitting a broad role into specific ones (code_simple vs code_complex) is
 * a CONFIGURATION action, and routing already honours it. */
void test_declared_roles_route_precisely(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));

   cfg.agent_count = 2;

   /* Specialist: narrow roles AND an explicit exec_roles list. */
   strcpy(cfg.agents[0].name, "simple_specialist");
   strcpy(cfg.agents[0].provider, "anthropic");
   strcpy(cfg.agents[0].model, "claude-haiku-4-5");
   strcpy(cfg.agents[0].roles[0], "code_simple");
   cfg.agents[0].role_count = 1;
   strcpy(cfg.agents[0].exec_roles[0], "code_simple");
   cfg.agents[0].exec_role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 0; /* cheapest, so only role filtering can exclude it */
   strcpy(cfg.agents[0].api_key, "k");

   strcpy(cfg.agents[1].name, "complex_specialist");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "code_complex");
   cfg.agents[1].role_count = 1;
   strcpy(cfg.agents[1].exec_roles[0], "code_complex");
   cfg.agents[1].exec_role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 3; /* dearest: chosen only because the role demands it */
   strcpy(cfg.agents[1].api_key, "k");

   /* Each role reaches exactly its specialist — including the DEARER one, which
    * cheapest-first would never pick if role filtering were inert. */
   assert(agent_route(&cfg, "code_simple") == &cfg.agents[0]);
   assert(agent_route(&cfg, "code_complex") == &cfg.agents[1]);
   assert(agent_route_with_caps(&cfg, "code_simple", &sys_cfg, 0, 0) == &cfg.agents[0]);
   assert(agent_route_with_caps(&cfg, "code_complex", &sys_cfg, 0, 0) == &cfg.agents[1]);

   /* A role neither agent declares is served by NEITHER. Routing is declared-role
    * only, so a built-in exec role is no more privileged than any other name. */
   assert(agent_route(&cfg, "review") == NULL);

   /* Exec-role membership does NOT grant routing. `review` remains a default exec
    * role (it still governs tool exposure), so agent_is_exec_role reports it once
    * the explicit exec list is cleared — but that never makes the agent a review
    * SEAT. Selection needs the `review` role (or `all`) in `roles`. */
   cfg.agents[0].exec_role_count = 0;
   assert(agent_is_exec_role(&cfg.agents[0], "review") == 1);
   assert(agent_route(&cfg, "review") == NULL);

   /* And the "all" wildcard in `roles` re-opens everything, so a fine-grained
    * deployment must avoid it. */
   strcpy(cfg.agents[0].roles[0], "all");
   strcpy(cfg.agents[0].exec_roles[0], "code_simple");
   cfg.agents[0].exec_role_count = 1;
   assert(agent_route(&cfg, "code_complex") == &cfg.agents[0]); /* wildcard wins on tier */

   printf("  PASS: test_declared_roles_route_precisely\n");
}

/* A declared per-agent SCOPE CEILING is how "our local delegates can do some
 * coding tasks, but not the complex ones" is expressed. Without it a local model
 * at cost_tier 0 wins EVERY packet under cheapest-first routing. */
void test_scope_ceiling_matches_work_to_capability(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   cfg.agent_count = 2;

   /* Free local seat, cheapest tier, but only good for bounded work. */
   strcpy(cfg.agents[0].name, "local");
   strcpy(cfg.agents[0].provider, "llama_native");
   strcpy(cfg.agents[0].model, "local-small");
   strcpy(cfg.agents[0].endpoint, "http://localhost:8080/v1");
   strcpy(cfg.agents[0].auth_type, "none");
   strcpy(cfg.agents[0].roles[0], "code");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].max_scope = AGENT_SCOPE_BOUNDED;

   /* Capable paid seat, dearest tier, no ceiling. */
   strcpy(cfg.agents[1].name, "capable");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "code");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 3;
   strcpy(cfg.agents[1].api_key, "k");

   /* BOUNDED work goes to the cheap local seat — the whole point of registering it. */
   assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 0, AGENT_SCOPE_BOUNDED) ==
          &cfg.agents[0]);

   /* WHOLE_TASK work must NOT: the local ceiling excludes it, so the dearer
    * capable seat wins despite cheapest-first. */
   assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 0, AGENT_SCOPE_WHOLE_TASK) ==
          &cfg.agents[1]);

   /* An UNDECLARED packet scope resolves to WHOLE_TASK: under uncertainty
    * over-select toward capability rather than risk a misplacement. */
   assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 0, AGENT_SCOPE_UNSET) ==
          &cfg.agents[1]);
   assert(agent_route_with_caps(&cfg, "code", &sys_cfg, 0, 0) == &cfg.agents[1]);

   /* THE TRAP: scope binds like a capability, NOT like min_context. Escalation
    * relaxes a context shortfall but must never relax a ceiling — otherwise a
    * whole_task packet escalates INTO the seat declared unable to handle it.
    * With the capable seat gone, the answer is "no route", not "use local". */
   cfg.agents[1].enabled = 0;
   assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 5000000,
                                       AGENT_SCOPE_WHOLE_TASK) == NULL);
   /* ...while bounded work still routes, so this is a ceiling and not an outage. */
   assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 0, AGENT_SCOPE_BOUNDED) ==
          &cfg.agents[0]);

   /* A fleet where EVERY agent declares a ceiling below whole_task cannot serve a
    * default-scope packet. That must WARN at config load, not surface later as a
    * confusing "no route" at dispatch — but it must not be fatal, since an
    * operator may legitimately run only bounded work. */
   {
      FILE *cf = fopen(agent_config_path(), "w");
      assert(cf != NULL);
      fputs("{\"agents\":[{\"name\":\"only_local\",\"provider\":\"ollama\","
            "\"endpoint\":\"http://localhost:11434\",\"model\":\"m\","
            "\"auth_type\":\"none\",\"max_scope\":\"bounded\","
            "\"roles\":[\"code\"]}]}\n",
            cf);
      fclose(cf);
      agent_config_t only;
      assert(agent_load_config(&only) == 0); /* warns, does NOT fail */
      assert(only.agent_count == 1);
      assert(only.agents[0].max_scope == AGENT_SCOPE_BOUNDED);
      /* And the ceiling round-trips through save. */
      assert(agent_save_config(&only) == 0);
      agent_config_t back;
      assert(agent_load_config(&back) == 0);
      assert(back.agents[0].max_scope == AGENT_SCOPE_BOUNDED);
      /* An unknown value is REJECTED rather than silently meaning "no ceiling",
       * which would hand the hardest work to the seat being limited. */
      cf = fopen(agent_config_path(), "w");
      assert(cf != NULL);
      fputs("{\"agents\":[{\"name\":\"x\",\"provider\":\"ollama\","
            "\"endpoint\":\"http://localhost:11434\",\"model\":\"m\","
            "\"auth_type\":\"none\",\"max_scope\":\"medium\","
            "\"roles\":[\"code\"]}]}\n",
            cf);
      fclose(cf);
      assert(agent_load_config(&back) != 0);
      unlink(agent_config_path());
   }

   /* The ceiling is CONFIGURATION eligibility, not model-metadata capability
    * routing, so it must bind with capability routing DISABLED as well. It
    * previously did not: agent_route_with_caps_inner returned the unscoped
    * router before scope was consulted, so a whole_task packet reached a
    * bounded-only seat the moment an operator set capability_routing=false. */
   {
      cfg.agents[1].enabled = 1; /* re-enable after the KIND-trap assertions above */
      agent_route_policy_t off_cfg;
      memset(&off_cfg, 0, sizeof(off_cfg));
      off_cfg.capability_routing = 0;
      assert(agent_route_with_caps_scoped(&cfg, "code", &off_cfg, 0, 0, AGENT_SCOPE_BOUNDED) ==
             &cfg.agents[0]);
      assert(agent_route_with_caps_scoped(&cfg, "code", &off_cfg, 0, 0, AGENT_SCOPE_WHOLE_TASK) ==
             &cfg.agents[1]);
      /* And a NULL sys_cfg must not be a bypass either. */
      assert(agent_route_with_caps_scoped(&cfg, "code", NULL, 0, 0, AGENT_SCOPE_WHOLE_TASK) ==
             &cfg.agents[1]);
   }

   printf("  PASS: test_scope_ceiling_matches_work_to_capability\n");
}

/* routing.prefer_local shifts work to FREE local seats when one is eligible —
 * but as an ORDERING preference only. It must never smuggle a packet past a
 * ceiling: local tokens are free, which removes the cost argument for
 * over-selecting, not the wall-clock one. */
void test_prefer_local_orders_but_never_bypasses(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;

   cfg.agent_count = 2;

   /* Local seat at a DEARER tier - the case that actually matters. Applying the
    * preference to the cheapest-tier list only would let a paid remote at tier 0
    * beat an eligible local at tier 1, the opposite of "try free local first".
    * An earlier version of this test set both to tier 0 and so proved nothing. */
   strcpy(cfg.agents[0].name, "local");
   strcpy(cfg.agents[0].provider, "ollama");
   strcpy(cfg.agents[0].model, "local-small");
   strcpy(cfg.agents[0].endpoint, "http://localhost:11434");
   strcpy(cfg.agents[0].auth_type, "none");
   strcpy(cfg.agents[0].roles[0], "code");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 1; /* DEARER than the remote seat */
   cfg.agents[0].max_scope = AGENT_SCOPE_BOUNDED;

   strcpy(cfg.agents[1].name, "remote");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "code");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 0; /* CHEAPER, so only the preference can lose it */
   strcpy(cfg.agents[1].api_key, "k");

   /* Off: cheapest-first wins, so the paid remote seat takes bounded work. */
   sys_cfg.prefer_local = 0;
   assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 0, AGENT_SCOPE_BOUNDED) ==
          &cfg.agents[1]);

   /* On: the local seat wins every time for work it can take. */
   sys_cfg.prefer_local = 1;
   for (int i = 0; i < 6; i++)
      assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 0, AGENT_SCOPE_BOUNDED) ==
             &cfg.agents[0]);

   /* But it must NOT bypass the ceiling: whole_task still goes remote even with
    * the preference on. This is the assertion that keeps "prefer free" from
    * becoming "misplace work". */
   for (int i = 0; i < 6; i++)
      assert(agent_route_with_caps_scoped(&cfg, "code", &sys_cfg, 0, 0, AGENT_SCOPE_WHOLE_TASK) ==
             &cfg.agents[1]);

   printf("  PASS: test_prefer_local_orders_but_never_bypasses\n");
}

/* A degraded seat must not beat a healthy one on price. This is the routing half
 * of the codex quota-outage fix: the breaker backoff keeps a hopeless seat out
 * of the routable set for longer, and this keeps a degraded-but-still-routable
 * seat from winning seat resolution while a healthy peer can serve the role. */
static const char *g_degraded_name = NULL;
static int route_test_is_degraded(const char *name)
{
   return g_degraded_name && name && strcmp(name, g_degraded_name) == 0;
}
static int route_test_degrade_all(const char *name)
{
   (void)name;
   return 1;
}

void test_prefer_healthy_over_degraded(void)
{
   agent_config_t cfg;
   agent_route_policy_t sys_cfg;
   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.capability_routing = 1;
   cfg.agent_count = 2;
   /* Assert the precondition rather than relying on the NULL default: a prior
    * test that left a degraded filter registered would otherwise corrupt this. */
   agent_set_route_degraded_filter(NULL);
   g_degraded_name = NULL;

   /* The CHEAPER seat is the degraded one - exactly codex's position: tier 0 and
    * flapping. Only the health preference can lose it to the dearer healthy seat. */
   strcpy(cfg.agents[0].name, "cheap-flaky");
   strcpy(cfg.agents[0].provider, "openai");
   strcpy(cfg.agents[0].model, "gpt-5.6-luna");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 0;
   strcpy(cfg.agents[0].api_key, "k");

   strcpy(cfg.agents[1].name, "dear-healthy");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-opus-4-8");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 1; /* dearer, so only the preference can win it */
   strcpy(cfg.agents[1].api_key, "k");

   /* No degraded filter (CLI/test default): cheapest wins, as before. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 0) == &cfg.agents[0]);

   /* With the cheaper seat degraded, the healthy dearer seat wins every time. */
   agent_set_route_degraded_filter(route_test_is_degraded);
   g_degraded_name = "cheap-flaky";
   for (int i = 0; i < 6; i++)
      assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 0) == &cfg.agents[1]);

   /* Fallback, not exclusion: when the degraded seat is the ONLY one that can
    * serve the role, it is still chosen - a degraded seat beats no seat, and the
    * preference must not turn a routable role into NULL. Make the healthy seat
    * INELIGIBLE by role rather than merely disabling it, so this proves routing
    * falls THROUGH to the degraded seat, not just that a disabled agent is
    * skipped. */
   strcpy(cfg.agents[1].roles[0], "code"); /* no longer serves "review" */
   /* Also pin explicit exec_roles: with none set, EVERY agent is exec-eligible
    * for the default exec roles (which include "review"), so a bare role change
    * would leave agents[1] still serving "review". */
   strcpy(cfg.agents[1].exec_roles[0], "code");
   cfg.agents[1].exec_role_count = 1;
   for (int i = 0; i < 6; i++)
      assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 0) == &cfg.agents[0]);
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].exec_role_count = 0;

   /* When EVERY seat is degraded, none can be preferred, so price decides again -
    * the preference narrows the field only when a healthy alternative exists. */
   agent_set_route_degraded_filter(route_test_degrade_all);
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 0) == &cfg.agents[0]);

   agent_set_route_degraded_filter(NULL);
   g_degraded_name = NULL;
   printf("  PASS: test_prefer_healthy_over_degraded\n");
}

/* Escalation target selection. An escalation is a placement CORRECTION, so it
 * must land on a genuinely dearer seat, must still respect the scope ceiling, and
 * must report "nothing" rather than re-running the class of seat that just failed. */
void test_escalation_target_selection(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 3;

   strcpy(cfg.agents[0].name, "cheap");
   strcpy(cfg.agents[0].provider, "anthropic");
   strcpy(cfg.agents[0].model, "claude-haiku-4-5");
   strcpy(cfg.agents[0].roles[0], "code");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   cfg.agents[0].cost_tier = 0;
   strcpy(cfg.agents[0].api_key, "k");

   strcpy(cfg.agents[1].name, "mid");
   strcpy(cfg.agents[1].provider, "anthropic");
   strcpy(cfg.agents[1].model, "claude-sonnet-5");
   strcpy(cfg.agents[1].roles[0], "code");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   cfg.agents[1].cost_tier = 2;
   strcpy(cfg.agents[1].api_key, "k");

   strcpy(cfg.agents[2].name, "dear");
   strcpy(cfg.agents[2].provider, "anthropic");
   strcpy(cfg.agents[2].model, "claude-opus-4-8");
   strcpy(cfg.agents[2].roles[0], "code");
   cfg.agents[2].role_count = 1;
   cfg.agents[2].enabled = 1;
   cfg.agents[2].tools_enabled = 1;
   cfg.agents[2].cost_tier = 3;
   cfg.agents[2].middleware.context_window = 400000; /* largest window */
   strcpy(cfg.agents[2].api_key, "k");

   /* Failing at tier 0 escalates to the MOST capable seat, not merely one step
    * up: the allowance is spent once, so there is no second chance to correct an
    * under-shoot, and over-selecting beats laddering. */
   agent_t *tgt = agent_route_escalation_target(&cfg, "code", 0, 0, AGENT_SCOPE_UNSET);
   assert(tgt == &cfg.agents[2]);

   /* The DEAREST seat wins even when a cheaper one has a bigger window. This
    * fixture is the realistic case, not a contrived one: `mid` is sonnet-class
    * and the catalog gives that family a larger window than the 400000 pinned on
    * the top seat, so ranking escalation by context window - as this did - sent
    * the correction to a seat of roughly the class that just failed. A bigger
    * window does not make a cheaper model better at work it already failed. */
   {
      int saved = cfg.agents[1].middleware.context_window;
      cfg.agents[1].middleware.context_window = 900000; /* out-windows `dear` */
      assert(agent_route_escalation_target(&cfg, "code", 0, 0, AGENT_SCOPE_UNSET) ==
             &cfg.agents[2]);
      cfg.agents[1].middleware.context_window = saved;
   }

   /* Never the same class of seat: a target must be strictly dearer. */
   assert(agent_route_escalation_target(&cfg, "code", 3, 0, AGENT_SCOPE_UNSET) == NULL);

   /* The SCOPE CEILING still binds during escalation. A placement correction is
    * not a licence to hand a packet to a seat declared unable to serve it. */
   cfg.agents[2].max_scope = AGENT_SCOPE_BOUNDED;
   tgt = agent_route_escalation_target(&cfg, "code", 0, 0, AGENT_SCOPE_WHOLE_TASK);
   assert(tgt == &cfg.agents[1]); /* dear is now ineligible; mid takes it */
   /* ...and bounded work can still reach it. */
   assert(agent_route_escalation_target(&cfg, "code", 0, 0, AGENT_SCOPE_BOUNDED) == &cfg.agents[2]);
   cfg.agents[2].max_scope = AGENT_SCOPE_UNSET;

   /* A disabled or role-mismatched seat is not a target. */
   cfg.agents[2].enabled = 0;
   assert(agent_route_escalation_target(&cfg, "code", 0, 0, AGENT_SCOPE_UNSET) == &cfg.agents[1]);
   cfg.agents[1].enabled = 0;
   assert(agent_route_escalation_target(&cfg, "code", 0, 0, AGENT_SCOPE_UNSET) == NULL);

   printf("  PASS: test_escalation_target_selection\n");
}

/* agent_default_primary must never hand back a disabled seat: a disabled
 * agents[0] (e.g. an unconfigured "claude") otherwise becomes the fallback
 * primary and every ingress request that doesn't name a model fast-fails as
 * "failed to reach the primary provider". */
void test_agent_default_primary_skips_disabled(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 3;
   strcpy(cfg.agents[0].name, "claude"); /* disabled fallback footgun */
   cfg.agents[0].enabled = 0;
   strcpy(cfg.agents[1].name, "minimax");
   cfg.agents[1].enabled = 1;
   strcpy(cfg.agents[2].name, "codex");
   cfg.agents[2].enabled = 1;

   /* No default set → first ENABLED agent, not the disabled agents[0]. */
   cfg.default_agent[0] = '\0';
   assert(agent_default_primary(&cfg) == &cfg.agents[1]);

   /* An enabled explicit default wins. */
   strcpy(cfg.default_agent, "codex");
   assert(agent_default_primary(&cfg) == &cfg.agents[2]);

   /* A disabled explicit default is ignored → first enabled agent. */
   strcpy(cfg.default_agent, "claude");
   assert(agent_default_primary(&cfg) == &cfg.agents[1]);

   /* Nothing enabled → NULL (caller reports a clear 503, not a phantom route). */
   cfg.agents[1].enabled = 0;
   cfg.agents[2].enabled = 0;
   cfg.default_agent[0] = '\0';
   assert(agent_default_primary(&cfg) == NULL);

   printf("  PASS: test_agent_default_primary_skips_disabled\n");
}
