#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "model_registry.h"

void test_agent_route_with_caps_honors_tools_enabled(void)
{
   agent_config_t cfg;
   config_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.model_meta_capability_routing = 1;

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
   config_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.model_meta_capability_routing = 1;

   cfg.agent_count = 1;
   strcpy(cfg.default_agent, "small-ctx");
   strcpy(cfg.agents[0].name, "small-ctx");
   strcpy(cfg.agents[0].provider, "openai");
   /* gpt-4's catalog context window is 8192 — below the 50000 requirement. */
   strcpy(cfg.agents[0].model, "gpt-4");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   strcpy(cfg.agents[0].api_key, "test-key");

   /* Catalog says 8192 < 50000 and no override -> dropped by the gate. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == NULL);

   /* An explicit per-agent override supersedes the catalog value and routes,
    * with no change to the model registry table. */
   cfg.agents[0].enabled = 1;
   cfg.agents[0].middleware.context_window = 1000000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[0]);

   /* An override below the requirement is still rejected. */
   cfg.agents[0].enabled = 1;
   cfg.agents[0].middleware.context_window = 1000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == NULL);
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
   assert(cap.context_window == 200000 || cap.context_window >= 1000000);
   assert(model_max_output(agent_catalog_provider(cl), cl->model) > 8192);

   assert(model_capability_get(agent_catalog_provider(cx), cx->model, &cap) != 0);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(model_max_output(agent_catalog_provider(cx), cx->model) > 8192);

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
   config_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.model_meta_capability_routing = 1;

   cfg.agent_count = 1;
   strcpy(cfg.agents[0].name, "unknown-ctx");
   strcpy(cfg.agents[0].provider, "anthropic");
   /* A model id no catalog or prefix table knows: context window resolves 0. */
   strcpy(cfg.agents[0].model, "totally-unknown-model-xyz");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   strcpy(cfg.agents[0].api_key, "test-key");

   /* Unknown window + a real requirement -> excluded (previously admitted). */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == NULL);

   /* No context requirement at all -> still routable. */
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
   assert(model_context_window("MiniMax-M2") == 200000);
   assert(model_context_window("kimi-k2.7-code") == 262144);
   /* The bare fallback still resolves the oldest known family, never a newer. */
   assert(model_context_window("minimax") == 200000);

   printf("  PASS: test_context_window_table_covers_live_vendors\n");
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
