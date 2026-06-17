/* test_agent_apikey.c: agents.json must never persist a resolved secret.
 *
 * Split out of test_agent.c (2000-line hard limit), mirroring its link line.
 *
 * A "$VAR" api_key is a reference, not a secret: it is resolved into the runtime
 * agent_t.api_key at load, but the verbatim reference is preserved in
 * api_key_disk so a save re-serializes the reference, not the expanded value.
 * Without this, any agent-config mutation (add/enable/remove) would rewrite the
 * loaded secret back into agents.json as plaintext. */
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

static void test_apikey_ref_not_serialized(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);

   setenv("AIMEE_APIKEY_REF_TEST", "sk-super-secret-value", 1);

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
   assert(strcmp(ag->api_key, "sk-super-secret-value") == 0);       /* resolved at runtime */
   assert(strcmp(ag->api_key_disk, "$AIMEE_APIKEY_REF_TEST") == 0); /* reference preserved */

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
   assert(ag2 != NULL && strcmp(ag2->api_key, "sk-super-secret-value") == 0);

   unsetenv("AIMEE_APIKEY_REF_TEST");
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
            /* mistral => not reasoning; no timeout_ms => standard default */
            "{\"name\":\"plain\",\"provider\":\"mistral\",\"model\":\"mistral-medium-latest\","
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

int main(void)
{
   char tmp_template[] = "/tmp/aimee-agent-apikey-XXXXXX";
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
   printf("agent_apikey: all tests passed\n");
   return 0;
}
