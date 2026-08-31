/* test_agent_apikey.c: agents.json must never persist a resolved secret.
 *
 * Split out of test_agent.c (2000-line hard limit), mirroring its link line.
 *
 * A "$VAR" api_key is first-boot routing metadata, not a secret. Bootstrap has
 * already ingested and unset the environment value before config is loaded; the
 * locked Vault-backed runtime cache supplies the value here. The verbatim
 * reference is preserved only until server startup migrates it to the agent's
 * Vault slot. A config save must never write the resolved value. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "config.h"
#include "platform_path.h"
#include "runtime_secret.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void test_apikey_ref_not_serialized(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);

   unsetenv("AIMEE_APIKEY_REF_TEST");
   assert(runtime_secret_store("AIMEE_APIKEY_REF_TEST", "sk-super-secret-value") == 0);

   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs("{\"agents\":[{\"name\":\"reftest\",\"endpoint\":\"https://api.example/v1\","
            "\"model\":\"m\",\"roles\":[\"code\"],"
            "\"api_key\":\"$AIMEE_APIKEY_REF_TEST\"}]}\n",
            f);
      fclose(f);
   }

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   agent_t *ag = agent_find(&loaded, "reftest");
   assert(ag != NULL);
   /* Config keeps the REFERENCE, in both fields: it does not resolve credentials,
    * so the cached registry (copied per lookup, resident for the process's life)
    * never holds a secret. */
   assert(strcmp(ag->api_key, "$AIMEE_APIKEY_REF_TEST") == 0);
   assert(strcmp(ag->api_key_disk, "$AIMEE_APIKEY_REF_TEST") == 0);

   /* The secret is reachable, at the point of use, from the vault module. */
   {
      char secret[MAX_API_KEY_LEN];
      assert(agent_api_key_secret(ag, secret, sizeof(secret)) == 1);
      assert(strcmp(secret, "sk-super-secret-value") == 0);
      runtime_secret_wipe(secret, sizeof(secret));
   }

   /* Save, then read the raw file: it must keep the $VAR ref, not the secret. */
   assert(agent_save_config(&loaded) == 0);
   {
      FILE *f = fopen(agent_config_path(), "r");
      assert(f != NULL);
      char buf[8192];
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[n] = '\0';
      assert(strstr(buf, "$AIMEE_APIKEY_REF_TEST") != NULL); /* reference written */
      assert(strstr(buf, "sk-super-secret-value") == NULL);  /* secret NOT written */
   }

   /* Reload still resolves to the secret. */
   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   agent_t *ag2 = agent_find(&reloaded, "reftest");
   assert(ag2 != NULL && strcmp(ag2->api_key, "$AIMEE_APIKEY_REF_TEST") == 0);
   {
      char secret[MAX_API_KEY_LEN];
      assert(agent_api_key_secret(ag2, secret, sizeof(secret)) == 1);
      assert(strcmp(secret, "sk-super-secret-value") == 0);
      runtime_secret_wipe(secret, sizeof(secret));
   }

   runtime_secret_remove("AIMEE_APIKEY_REF_TEST");
   printf("  PASS: test_apikey_ref_not_serialized\n");
}

/* The save fallback (api_key_disk empty -> write api_key) must emit the
 * reference, never a resolved secret. An in-memory agent created without a load
 * (e.g. `agent add $VAR`) holds the unexpanded reference in api_key. */
static void test_apikey_ref_fallback_save(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   agent_t *ag = &cfg.agents[0];
   snprintf(ag->name, sizeof(ag->name), "memref");
   snprintf(ag->endpoint, sizeof(ag->endpoint), "https://api.example/v1");
   snprintf(ag->model, sizeof(ag->model), "m");
   ag->enabled = 1;
   snprintf(ag->api_key, sizeof(ag->api_key), "$AIMEE_APIKEY_FALLBACK_TEST");
   /* api_key_disk intentionally left empty (zeroed by memset). */

   assert(agent_save_config(&cfg) == 0);
   FILE *f = fopen(agent_config_path(), "r");
   assert(f != NULL);
   char buf[8192];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   assert(strstr(buf, "$AIMEE_APIKEY_FALLBACK_TEST") != NULL); /* reference via fallback */
   printf("  PASS: test_apikey_ref_fallback_save\n");
}

/* Regression (also split out of test_agent.c at the 2000-line limit): a delegate
 * must never reach the HTTP layer with a non-positive timeout. timeout_ms <= 0
 * disables the read deadline (conn_open deadline_ns=0) and a stalled provider
 * hangs the worker forever, leaking its pool thread + concurrency slot until the
 * whole background-delegate queue wedges. */
static void test_delegate_effective_timeout(void)
{
   /* Explicit request timeout always wins. */
   assert(delegate_effective_timeout_ms(30000, 180000) == 30000);
   assert(delegate_effective_timeout_ms(5000, 0) == 5000);
   /* No request timeout: fall back to the agent's configured timeout. */
   assert(delegate_effective_timeout_ms(0, 180000) == 180000);
   assert(delegate_effective_timeout_ms(-1, 120000) == 120000);
   /* THE BUG: neither request nor agent configures a timeout (agents with no
    * timeout_ms zero-init to 0). Must resolve to the default ceiling, NEVER 0. */
   assert(delegate_effective_timeout_ms(0, 0) == AGENT_DEFAULT_TIMEOUT_MS);
   assert(delegate_effective_timeout_ms(-1, -1) == AGENT_DEFAULT_TIMEOUT_MS);
   assert(delegate_effective_timeout_ms(0, 0) > 0);

   /* A workflow stage cap may only shorten the whole four-call loop. It must
    * preserve a smaller configured budget and saturate multiplication rather
    * than overflowing a very large per-call timeout. */
   assert(agent_loop_total_timeout_ms(180000, 0) == 720000);
   assert(agent_loop_total_timeout_ms(180000, 300000) == 300000);
   assert(agent_loop_total_timeout_ms(60000, 300000) == 240000);
   assert(agent_loop_total_timeout_ms(INT_MAX, 0) == INT_MAX);
   assert(agent_timeout_cap_ms(900000, 600000) == 600000);
   assert(agent_timeout_cap_ms(180000, 600000) == 180000);
   assert(agent_timeout_cap_ms(-1, 600000) == 600000);
}

/* otel stubs: the route-health / agent-loop paths exercised below reach
 * agent_trace_log, which references these. The real otel.o isn't in this
 * target's link line, so provide no-op definitions (mirrors test_agent.c). */
void otel_init(const char *endpoint, const char *service_name, const char *session)
{
   (void)endpoint;
   (void)service_name;
   (void)session;
}
void otel_on_trace(const char *direction, const char *tool_name, const char *tool_args,
                   const char *tool_result, int turn)
{
   (void)direction;
   (void)tool_name;
   (void)tool_args;
   (void)tool_result;
   (void)turn;
}

/* Route-health filter predicates for test_agent_route_health_filter. */
static const char *g_test_down_agent = NULL;
static int test_route_filter_named(const char *name)
{
   return g_test_down_agent && strcmp(name, g_test_down_agent) == 0;
}
static int test_route_filter_all(const char *name)
{
   (void)name;
   return 1;
}

/* A provider the health catalog marks DOWN must be excluded from routing so
 * new work falls back to a healthy peer; when every candidate is down, routing
 * returns NULL (a clean failure) rather than handing work to a dead endpoint. */
static void test_agent_route_health_filter(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   strcpy(cfg.agents[0].name, "cheap");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;

   strcpy(cfg.agents[1].name, "expensive");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;

   /* No filter registered: cheapest healthy agent wins (baseline). */
   agent_set_route_health_filter(NULL);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);

   /* Cheap agent DOWN: it is no longer available, routing uses the peer. */
   g_test_down_agent = "cheap";
   agent_set_route_health_filter(test_route_filter_named);
   assert(!agent_is_available_for_routing(&cfg.agents[0]));
   assert(agent_is_available_for_routing(&cfg.agents[1]));
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);

   /* Every candidate DOWN: clean NULL, never a dead-endpoint wedge. */
   agent_set_route_health_filter(test_route_filter_all);
   assert(agent_route(&cfg, "summarize") == NULL);

   /* Clearing the filter restores the prior behaviour exactly. */
   g_test_down_agent = NULL;
   agent_set_route_health_filter(NULL);
   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);
}

/* Regression: the multi-turn tool loop must not issue a model call with a
 * starved (sub-viable) timeout when its budget is nearly exhausted -- that
 * yields an HTTP -1 read failure that gets misreported as "provider
 * unreachable" and marks the provider degraded. agent_loop_per_call_timeout_ms
 * returns -1 instead so the loop stops cleanly. */
static void test_agent_loop_per_call_timeout(void)
{
   /* Early in the loop: full per-call timeout. */
   assert(agent_loop_per_call_timeout_ms(180000, 720000, 0) == 180000);
   /* Mid loop: capped to the remaining budget, still >= the viable floor. */
   assert(agent_loop_per_call_timeout_ms(180000, 720000, 660000) == 60000);
   /* Budget nearly gone (only 2.2s left -- the real-world failure): stop, do
    * NOT issue a doomed 2262ms call. */
   assert(agent_loop_per_call_timeout_ms(180000, 720000, 717738) == -1);
   /* Exactly at the floor is still viable; one below it stops. */
   assert(agent_loop_per_call_timeout_ms(180000, 720000, 660000) == 60000);
   assert(agent_loop_per_call_timeout_ms(180000, 720000, 660001) == -1);
   /* Small configured timeout: the floor never exceeds agent_timeout_ms. */
   assert(agent_loop_per_call_timeout_ms(10000, 40000, 0) == 10000);
   assert(agent_loop_per_call_timeout_ms(10000, 40000, 35000) == -1);

   /* A workflow stage cap can be SMALLER than one viable call, so the very first
    * call is already non-viable. Production numbers: a 180s agent capped to the
    * 57759ms left in its stage. The loop used to start anyway and report the
    * budget as exhausted after 97ms; agent_execute_with_tools_internal now
    * preflights this exact condition and refuses with an honest reason. */
   assert(agent_loop_total_timeout_ms(180000, 57759) == 57759);
   assert(agent_loop_per_call_timeout_ms(180000, 57759, 0) == -1);
   /* One millisecond over the floor is viable, so the preflight must not fire. */
   assert(agent_loop_per_call_timeout_ms(180000, 60000, 0) == 60000);

   /* The predicate the runtime actually branches on. */
   assert(agent_loop_window_too_small(180000, 57759) == 1);
   assert(agent_loop_window_too_small(180000, 60000) == 0);
   assert(agent_loop_window_too_small(180000, 720000) == 0);
}

static void test_claude_cli_predicate(void)
{
   agent_t a;

   /* claude via tmux/CLI login → gated (primary-only by default). */
   memset(&a, 0, sizeof(a));
   snprintf(a.backend, sizeof(a.backend), "%s", AGENT_BACKEND_TMUX_CLI);
   snprintf(a.cli_kind, sizeof(a.cli_kind), "claude");
   assert(agent_is_claude_cli(&a) == 1);
   snprintf(a.cli_kind, sizeof(a.cli_kind), "claude-code");
   assert(agent_is_claude_cli(&a) == 1);
   snprintf(a.backend, sizeof(a.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(a.cli_kind, sizeof(a.cli_kind), "claude");
   assert(agent_is_claude_cli(&a) == 1);

   /* Claude-only: other CLI agents (Codex CLI, gemini-cli) are NOT gated. */
   snprintf(a.cli_kind, sizeof(a.cli_kind), "codex");
   assert(agent_is_claude_cli(&a) == 0);
   snprintf(a.cli_kind, sizeof(a.cli_kind), "gemini");
   assert(agent_is_claude_cli(&a) == 0);

   /* plain HTTP/API-key agent → not gated. */
   memset(&a, 0, sizeof(a));
   snprintf(a.backend, sizeof(a.backend), "openai");
   assert(agent_is_claude_cli(&a) == 0);
}

/* primary_only migration + persistence. A legacy agents.json (written before the
 * field existed) has no `primary_only` key: a claude-CLI agent must migrate to
 * primary-only (matching the removed global claude_cli_delegate_enabled default),
 * every other agent to delegate-eligible. An explicit false must then survive a
 * save->reload (agent_save_config always writes the key), so unchecking Primary
 * Agent Only on a claude agent sticks instead of re-defaulting to true. */
static void test_primary_only_migration_and_persist(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);
   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      /* Legacy file: NEITHER agent carries primary_only. */
      fputs("{\"agents\":["
            "{\"name\":\"claude\",\"provider\":\"claude\",\"backend\":\"tmux-cli\","
            "\"cli_kind\":\"claude\",\"roles\":[\"code\"]},"
            "{\"name\":\"minimax\",\"provider\":\"anthropic\",\"model\":\"MiniMax-M3\","
            "\"endpoint\":\"https://api.minimax.io/v1/chat/completions\",\"roles\":[\"all\"]}]}\n",
            f);
      fclose(f);
   }
   agent_config_t cfg;
   assert(agent_load_config(&cfg) == 0);
   agent_t *claude = agent_find(&cfg, "claude");
   agent_t *minimax = agent_find(&cfg, "minimax");
   assert(claude && minimax);
   /* Migration: absent key -> claude-CLI ON, everything else OFF. */
   assert(claude->primary_only == 1);
   assert(minimax->primary_only == 0);

   /* Operator unchecks Primary Agent Only for claude; it must persist. */
   claude->primary_only = 0;
   assert(agent_save_config(&cfg) == 0);
   /* Force a genuine file re-parse (agent_save_config also refreshes the in-memory
    * cache, which would otherwise mask a parse/migration bug). */
   setenv("AIMEE_NO_CACHE", "1", 1);
   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   unsetenv("AIMEE_NO_CACHE");
   agent_t *claude2 = agent_find(&reloaded, "claude");
   assert(claude2 != NULL);
   /* Explicit false survived: the key was written and NOT re-migrated to true. */
   assert(claude2->primary_only == 0);
   printf("  PASS: test_primary_only_migration_and_persist\n");
}

/* A reasoning model with no operator timeout gets the higher reasoning default;
 * a non-reasoning model keeps the standard default; an explicit value always wins. */
static void test_reasoning_timeout_default(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);
   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs("{\"agents\":["
            /* minimax => MODEL_CAP_REASONING; no timeout_ms => reasoning default */
            "{\"name\":\"rsn\",\"provider\":\"minimax\",\"model\":\"MiniMax-M3\","
            "\"endpoint\":\"https://api.minimax.io/v1/chat/completions\",\"roles\":[\"review\"]},"
            /* A model the CATALOG marks non-reasoning => standard default.
             * mistral-medium-latest used to sit here, chosen while the bundled
             * snapshot was unreachable and the heuristic answered instead; the
             * real catalog marks it reasoning:true, so it stopped testing the
             * non-reasoning branch the moment the snapshot became readable. */
            "{\"name\":\"plain\",\"provider\":\"mistral\",\"model\":\"ministral-8b-latest\","
            "\"endpoint\":\"https://api.mistral.ai/v1/chat/completions\",\"roles\":[\"review\"]},"
            /* explicit timeout always wins, even for a reasoning model */
            "{\"name\":\"pinned\",\"provider\":\"minimax\",\"model\":\"MiniMax-M3\","
            "\"endpoint\":\"https://api.minimax.io/v1/chat/completions\",\"timeout_ms\":5000,"
            "\"roles\":[\"review\"]}]}\n",
            f);
      fclose(f);
   }
   agent_config_t cfg;
   assert(agent_load_config(&cfg) == 0);
   agent_t *rsn = agent_find(&cfg, "rsn");
   agent_t *plain = agent_find(&cfg, "plain");
   agent_t *pinned = agent_find(&cfg, "pinned");
   assert(rsn && plain && pinned);
   assert(rsn->timeout_ms == AGENT_REASONING_TIMEOUT_MS);
   assert(plain->timeout_ms == AGENT_DEFAULT_TIMEOUT_MS);
   assert(pinned->timeout_ms == 5000);
   printf("  PASS: test_reasoning_timeout_default\n");
}

/* A declared PRICE survives a save even when it is 0, and an undeclared field
 * stays absent. Capacities deliberately differ -- see the assertions below.
 *
 * The old convention was "0 = unset, fall back to the catalog", and the
 * serializer wrote a price only when it was > 0. That silently discarded a
 * deliberate 0 on the first save, so a free or subscription-priced seat
 * reverted to "unset" -- harmless only while a catalog sat underneath to
 * answer. With operator declaration as the authoritative source there is
 * nothing underneath, so the two states must stay distinct across a round
 * trip. */
static void test_declared_zero_round_trips(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);

   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      /* "free" declares 0 on every axis; "quiet" declares nothing at all. */
      fputs("{\"agents\":["
            "{\"name\":\"free\",\"endpoint\":\"https://api.example/v1\",\"model\":\"m\","
            "\"roles\":[\"code\"],\"price_in_per_mtok\":0,\"price_out_per_mtok\":0,"
            "\"context_window\":0,\"max_output\":0},"
            "{\"name\":\"quiet\",\"endpoint\":\"https://api.example/v1\",\"model\":\"m\","
            "\"roles\":[\"code\"]}]}\n",
            f);
      fclose(f);
   }

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   agent_t *freebie = agent_find(&loaded, "free");
   agent_t *quiet = agent_find(&loaded, "quiet");
   assert(freebie && quiet);

   /* Declared-as-zero is a declaration; silence is not. Both read 0 as a value,
    * so only the bits tell them apart -- which is the point. */
   assert(freebie->declared & AGENT_DECL_PRICE_IN);
   assert(freebie->declared & AGENT_DECL_PRICE_OUT);
   assert(freebie->price_in_per_mtok == 0.0);
   assert(quiet->declared == 0);

   assert(agent_save_config(&loaded) == 0);

   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   agent_t *free2 = agent_find(&reloaded, "free");
   agent_t *quiet2 = agent_find(&reloaded, "quiet");
   assert(free2 && quiet2);

   /* The PRICE declaration survived the save rather than being dropped for
    * being 0 -- that is the case a bare number cannot express. */
   assert(free2->declared & AGENT_DECL_PRICE_IN);
   assert(free2->declared & AGENT_DECL_PRICE_OUT);
   assert(free2->price_in_per_mtok == 0.0);
   /* CAPACITIES are the opposite and deliberately do NOT round-trip a 0: there
    * is no zero-token window, so 0 is the absence of a statement, and writing
    * one back would only make configs noisier without telling a reader anything.
    * Capacity resolution keys on the value for the same reason. */
   assert(free2->middleware.context_window == 0);
   assert(free2->max_output == 0);
   /* ...and silence was not turned into a declaration by the round trip. */
   assert(quiet2->declared == 0);
}

/* The legacy spelling -- context_window nested inside "middleware" -- still
 * loads, and is rewritten at the top level so a config migrates forward on its
 * first save rather than carrying two spellings that can drift apart. */
static void test_legacy_middleware_context_window_migrates(void)
{
   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs("{\"agents\":[{\"name\":\"legacy\",\"endpoint\":\"https://api.example/v1\","
            "\"model\":\"m\",\"roles\":[\"code\"],"
            "\"middleware\":{\"context_window\":272000}}]}\n",
            f);
      fclose(f);
   }

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   agent_t *ag = agent_find(&loaded, "legacy");
   assert(ag != NULL);
   assert(ag->middleware.context_window == 272000);
   assert(ag->declared & AGENT_DECL_CONTEXT_WINDOW);

   assert(agent_save_config(&loaded) == 0);
   {
      FILE *f = fopen(agent_config_path(), "r");
      assert(f != NULL);
      char buf[8192] = {0};
      size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      assert(nread > 0);
      /* Written once, at the top level; the nested spelling is not re-emitted. */
      assert(strstr(buf, "\"context_window\":\t272000") != NULL ||
             strstr(buf, "\"context_window\": 272000") != NULL ||
             strstr(buf, "\"context_window\":272000") != NULL);
      assert(strstr(buf, "\"middleware\"") == NULL);
   }

   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   agent_t *ag2 = agent_find(&reloaded, "legacy");
   assert(ag2 && ag2->middleware.context_window == 272000);
   assert(ag2->declared & AGENT_DECL_CONTEXT_WINDOW);
}

/* The migration off the bundled catalog: an agent that states no limits adopts
 * the catalog's published figures as its own, once, at load. That is what lets
 * the snapshot eventually be deleted -- every consumer reads the agent, so the
 * agent has to carry the numbers it has actually been running on. */
static void test_catalog_limits_migrate_into_the_agent(void)
{
   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      /* "known" is in the bundled catalog; "guessy" is a name nothing publishes. */
      fputs("{\"agents\":["
            "{\"name\":\"known\",\"endpoint\":\"https://api.anthropic.com/v1\","
            "\"model\":\"claude-sonnet-4-6\",\"provider\":\"anthropic\",\"roles\":[\"code\"]},"
            "{\"name\":\"guessy\",\"endpoint\":\"https://x/v1\","
            "\"model\":\"no-such-model-qqq\",\"provider\":\"openai\",\"roles\":[\"code\"]}]}\n",
            f);
      fclose(f);
   }

   agent_config_t cfg;
   assert(agent_load_config(&cfg) == 0);
   agent_t *known = agent_find(&cfg, "known");
   agent_t *guessy = agent_find(&cfg, "guessy");
   assert(known && guessy);

   /* Adopted, and marked as the agent's own so it survives a save. */
   assert(known->middleware.context_window == 1000000);
   assert(known->max_output == 128000);
   assert(known->declared & AGENT_DECL_CONTEXT_WINDOW);

   /* NOT adopted. model_capability_get() would have answered here via its
    * heuristic, which infers a window for any unknown name and always succeeds;
    * writing that into config would stamp a guess in as though the operator had
    * chosen it. The migration reads the catalog only, so an unpublished model
    * stays honestly unknown. */
   assert(guessy->middleware.context_window == 0);
   assert(guessy->max_output == 0);
   assert(guessy->declared == 0);

   /* Idempotent, and it persists: save, reload, same answer and nothing new. */
   assert(agent_save_config(&cfg) == 0);
   agent_config_t again;
   assert(agent_load_config(&again) == 0);
   agent_t *known2 = agent_find(&again, "known");
   assert(known2 && known2->middleware.context_window == 1000000);
   assert(agent_find(&again, "guessy")->declared == 0);
}

/* An operator's own figure is never overwritten by the catalog. The catalog is
 * being removed precisely because it is not authoritative. */
static void test_migration_never_overwrites_a_stated_limit(void)
{
   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs("{\"agents\":[{\"name\":\"pinned\",\"endpoint\":\"https://api.anthropic.com/v1\","
            "\"model\":\"claude-sonnet-4-6\",\"provider\":\"anthropic\",\"roles\":[\"code\"],"
            "\"context_window\":200000}]}\n",
            f);
      fclose(f);
   }
   agent_config_t cfg;
   assert(agent_load_config(&cfg) == 0);
   agent_t *ag = agent_find(&cfg, "pinned");
   /* A deliberate policy ceiling below the model's capability stands. */
   assert(ag && ag->middleware.context_window == 200000);
}

int main(void)
{
   char tmp_template[256];
   snprintf(tmp_template, sizeof tmp_template, "%s/aimee-agent-apikey-XXXXXX", platform_tmpdir());
   char *tmp_home = mkdtemp(tmp_template);
   assert(tmp_home != NULL);
   setenv("AIMEE_HOME", tmp_home, 1);

   test_apikey_ref_not_serialized();
   test_apikey_ref_fallback_save();
   test_delegate_effective_timeout();

   test_agent_route_health_filter();
   test_agent_loop_per_call_timeout();
   test_reasoning_timeout_default();
   test_claude_cli_predicate();
   test_primary_only_migration_and_persist();
   test_declared_zero_round_trips();
   test_legacy_middleware_context_window_migrates();
   test_catalog_limits_migrate_into_the_agent();
   test_migration_never_overwrites_a_stated_limit();
   printf("agent_apikey: all tests passed\n");
   return 0;
}
