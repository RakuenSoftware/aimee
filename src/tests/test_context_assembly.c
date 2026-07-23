#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "agent_exec.h"
#include "db.h"
#include "../db1/db1.h"
#include "../db2/db2.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/lifecycle.h"

static void setup(void)
{
   /* db1_init is idempotent: reuse a single in-memory db1 across all
    * tests. anti-pattern test cases clear the table at the start. */
   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();
   db2_set_ephemeral(1);
}

static void teardown(void)
{
   db2_test_shim_close();
}

static void test_null_hint_produces_same_as_original(void)
{
   setup();
   memory_t m;

   /* Insert some memories across kinds/tiers */
   memory_insert(TIER_L2, KIND_FACT, "db-host", "PostgreSQL at 10.0.0.5", 0.9, "s1", &m);
   memory_insert(TIER_L2, KIND_PREFERENCE, "style", "concise responses", 0.8, "s1", &m);
   memory_insert(TIER_L1, KIND_TASK, "deploy-api", "deploy the API service", 0.7, "s1", &m);
   memory_insert(TIER_L1, KIND_EPISODE, "fixed-auth", "fixed auth cert issue", 0.6, "s1", &m);
   memory_insert(TIER_L2, KIND_DECISION, "use-mTLS", "all services use mTLS", 0.95, "s1", &m);

   char *ctx = memory_assemble_context(NULL);
   assert(ctx != NULL);
   assert(strlen(ctx) > 0);

   /* Should have the standard section headers */
   assert(strstr(ctx, "# Memory Context") != NULL);
   assert(strstr(ctx, "## Key Facts") != NULL);
   assert(strstr(ctx, "## Constraints") != NULL);

   /* Should contain the inserted content */
   assert(strstr(ctx, "PostgreSQL") != NULL);
   assert(strstr(ctx, "mTLS") != NULL);

   free(ctx);
   teardown();
}

static void test_task_hint_prioritizes_relevant_memories(void)
{
   setup();
   memory_t m;

   /* Insert auth-related and unrelated facts */
   memory_insert(TIER_L2, KIND_FACT, "auth-config", "PostgreSQL cert auth uses client certificates",
                 0.9, "s1", &m);
   memory_insert(TIER_L2, KIND_FACT, "deploy-config", "deployments use blue-green strategy", 0.9,
                 "s1", &m);
   memory_insert(TIER_L2, KIND_FACT, "auth-flow", "authentication flow validates certificate chain",
                 0.5, "s1", &m);

   /* With auth-related task hint, auth memories should appear first */
   char *ctx = memory_assemble_context("fix PostgreSQL cert auth");
   assert(ctx != NULL);

   /* Auth-related content should be present */
   assert(strstr(ctx, "cert") != NULL);

   /* Both auth memories should appear before the deploy one */
   char *auth1 = strstr(ctx, "client certificates");
   char *auth2 = strstr(ctx, "certificate chain");
   char *deploy = strstr(ctx, "blue-green");

   /* At minimum the highest-scored auth memory should be there */
   assert(auth1 != NULL);

   /* If deploy appears at all, the strongest auth evidence should land later
    * in the context so it stays closest to the prompt. */
   if (deploy != NULL && auth1 != NULL)
      assert(auth1 > deploy);
   if (deploy != NULL && auth2 != NULL)
      assert(auth2 > deploy);

   free(ctx);
   teardown();
}

static void test_task_hint_respects_budget(void)
{
   setup();
   memory_t m;

   /* Insert many memories to test budget enforcement */
   for (int i = 0; i < 30; i++)
   {
      char key[64], content[256];
      snprintf(key, sizeof(key), "fact-%d", i);
      snprintf(content, sizeof(content), "this is fact number %d about auth configuration", i);
      memory_insert(TIER_L2, KIND_FACT, key, content, 0.9 - (i * 0.01), "s1", &m);
   }

   char *ctx = memory_assemble_context("auth configuration");
   assert(ctx != NULL);
   assert((int)strlen(ctx) <= MAX_CONTEXT_TOTAL + 256);

   free(ctx);
   teardown();
}

static void test_task_hint_fills_all_sections(void)
{
   setup();
   memory_t m;

   /* Insert memories of each kind that match the task hint */
   memory_insert(TIER_L2, KIND_FACT, "net-topology", "network uses VLAN isolation", 0.9, "s1", &m);
   memory_insert(TIER_L1, KIND_TASK, "fix-network", "fix network routing issue", 0.7, "s1", &m);
   memory_insert(TIER_L1, KIND_EPISODE, "network-outage", "network outage on March 15", 0.6, "s1",
                 &m);
   memory_insert(TIER_L2, KIND_DECISION, "network-policy", "network policy requires firewall rules",
                 0.95, "s1", &m);

   char *ctx = memory_assemble_context("network routing");
   assert(ctx != NULL);

   /* All four sections should be populated */
   assert(strstr(ctx, "## Key Facts") != NULL);
   assert(strstr(ctx, "## Active Tasks") != NULL);
   assert(strstr(ctx, "## Recent Context") != NULL);
   assert(strstr(ctx, "## Constraints") != NULL);

   /* Content from each section should be present */
   assert(strstr(ctx, "VLAN isolation") != NULL);
   assert(strstr(ctx, "routing issue") != NULL);
   assert(strstr(ctx, "outage") != NULL);
   assert(strstr(ctx, "firewall") != NULL);

   free(ctx);
   teardown();
}

static void test_empty_db_with_task_hint(void)
{
   setup();

   char *ctx = memory_assemble_context("anything");
   assert(ctx != NULL);
   assert(strstr(ctx, "# Memory Context") != NULL);
   /* No sections should appear */
   assert(strstr(ctx, "## Key Facts") == NULL);

   free(ctx);
   teardown();
}

static void test_graph_boost_integration(void)
{
   setup();
   memory_t m;

   /* Insert memories */
   memory_insert(TIER_L2, KIND_FACT, "spire-config", "SPIRE manages X.509 certificates for mTLS",
                 0.7, "s1", &m);
   memory_insert(TIER_L2, KIND_FACT, "unrelated-fact", "disk usage is monitored by Prometheus", 0.9,
                 "s1", &m);

   /* Create entity edge linking "spire" to "auth" */
   static const char *edge_sql = "INSERT INTO entity_edges (source, relation, target, weight)"
                                 " VALUES ('spire', 'provides', 'auth', 3)";
   char edge_err[128] = "";
   (void)aimee_pg_exec(db2_conn(), edge_sql, edge_err, sizeof(edge_err));

   /* Search for "auth" - spire should get a graph boost */
   char *ctx = memory_assemble_context("auth certificates");
   assert(ctx != NULL);

   /* SPIRE memory should appear due to graph boost even though
    * the unrelated fact has higher base confidence */
   assert(strstr(ctx, "SPIRE") != NULL);

   free(ctx);
   teardown();
}

static void test_task_hint_formats_xml_and_negative_context(void)
{
   setup();
   memory_t old_mem, new_mem, fact;
   memory_insert(TIER_L2, KIND_FACT, "transport",
                 "Use WebSockets for browser transport in the frontend.", 0.8, "s1", &old_mem);
   assert(memory_supersede(old_mem.id,
                           "Use server-sent events for browser transport in the frontend.", 0.95,
                           "s2", &new_mem) == 0);
   memory_insert(TIER_L2, KIND_FACT, "frontend facts",
                 "Frontend clients subscribe to event updates over HTTP streams.", 0.9, "s2",
                 &fact);
   anti_pattern_t ap;
   assert(db2_anti_pattern_insert("websocket reconnect loop",
                                  "Avoid stale websocket reconnect loops in browser transport",
                                  "test", "", 0.9, &ap) == 0);

   char *ctx = memory_assemble_context("frontend transport websocket reconnect");
   assert(ctx != NULL);
   assert(strstr(ctx, "<historical_fact") != NULL);
   assert(strstr(ctx, "<avoid_these_patterns>") != NULL);
   assert(strstr(ctx, "Outdated:") != NULL);
   assert(strstr(ctx, "Use instead:") != NULL);
   assert(strstr(ctx, "Avoid: Avoid stale websocket reconnect loops") != NULL);

   free(ctx);
   teardown();
}

/* --- Task type classification tests --- */

static void test_classify_bug_fix(void)
{
   assert(task_type_classify("fix the crash in auth module") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("Debug the error in login") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("Something is broken in deploy") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("Investigate the regression") == TASK_TYPE_BUG_FIX);
   assert(task_type_classify("The build fails on CI") == TASK_TYPE_BUG_FIX);
}

static void test_classify_refactor(void)
{
   assert(task_type_classify("refactor the auth module") == TASK_TYPE_REFACTOR);
   assert(task_type_classify("rename getUserData to fetchUser") == TASK_TYPE_REFACTOR);
   assert(task_type_classify("extract common logic into helper") == TASK_TYPE_REFACTOR);
   assert(task_type_classify("clean up the unused imports") == TASK_TYPE_REFACTOR);
}

static void test_classify_feature(void)
{
   assert(task_type_classify("add pagination to the API") == TASK_TYPE_FEATURE);
   assert(task_type_classify("implement rate limiting") == TASK_TYPE_FEATURE);
   assert(task_type_classify("create a new endpoint for users") == TASK_TYPE_FEATURE);
   assert(task_type_classify("build webhook support") == TASK_TYPE_FEATURE);
}

static void test_classify_review(void)
{
   assert(task_type_classify("review the PR changes") == TASK_TYPE_REVIEW);
   assert(task_type_classify("audit the security config") == TASK_TYPE_REVIEW);
   assert(task_type_classify("verify the deployment worked") == TASK_TYPE_REVIEW);
   assert(task_type_classify("validate the schema migration") == TASK_TYPE_REVIEW);
}

static void test_classify_test(void)
{
   assert(task_type_classify("test the auth flow") == TASK_TYPE_TEST);
   assert(task_type_classify("increase test coverage for db") == TASK_TYPE_TEST);
   assert(task_type_classify("write unit tests for parser") == TASK_TYPE_TEST);
}

static void test_classify_general(void)
{
   assert(task_type_classify("deploy the service") == TASK_TYPE_GENERAL);
   assert(task_type_classify("update the config") == TASK_TYPE_GENERAL);
   assert(task_type_classify(NULL) == TASK_TYPE_GENERAL);
   assert(task_type_classify("") == TASK_TYPE_GENERAL);
}

static void test_task_type_name_strings(void)
{
   assert(strcmp(task_type_name(TASK_TYPE_BUG_FIX), "bug_fix") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_REFACTOR), "refactor") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_FEATURE), "feature") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_REVIEW), "review") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_TEST), "test") == 0);
   assert(strcmp(task_type_name(TASK_TYPE_GENERAL), "general") == 0);
}

static void test_agent_exec_context_truncates_large_prompt(void)
{
   setup();
   const char *old_no_kb = getenv("AIMEE_CONTEXT_NO_KB");
   char *old_no_kb_copy = old_no_kb ? strdup(old_no_kb) : NULL;
   setenv("AIMEE_CONTEXT_NO_KB", "1", 1);

   size_t prompt_len = (size_t)(AGENT_CONTEXT_BUDGET * 3);
   char *prompt = malloc(prompt_len + 1);
   assert(prompt != NULL);
   memset(prompt, 'x', prompt_len);
   memcpy(prompt, "fix delegate review crash ", 26);
   prompt[prompt_len] = '\0';

   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "test-agent");
   snprintf(ag.model, sizeof(ag.model), "test-model");

   char *ctx = agent_build_exec_context(&ag, NULL, prompt);
   assert(ctx != NULL);
   assert(strlen(ctx) < (size_t)(AGENT_CONTEXT_BUDGET + 4096));
   assert(strstr(ctx, "You are an execution agent") != NULL);
   assert(strstr(ctx, "# Code Principles") != NULL);
   assert(strstr(ctx, "Prefer composition over inheritance") != NULL);

   free(ctx);
   free(prompt);
   if (old_no_kb_copy)
      setenv("AIMEE_CONTEXT_NO_KB", old_no_kb_copy, 1);
   else
      unsetenv("AIMEE_CONTEXT_NO_KB");
   free(old_no_kb_copy);
   teardown();
}

static void test_agent_exec_context_ex_can_skip_kb(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "ctx-sentinel",
                 "CTX_SENTINEL_SHOULD_NOT_APPEAR_IN_CURRENT_CODE_ONLY_CONTEXT", 0.99, "s1", &m);

   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "test-agent");
   snprintf(ag.model, sizeof(ag.model), "test-model");

   char *ctx = agent_build_exec_context_ex(&ag, NULL, "ctx-sentinel", 1);
   assert(ctx != NULL);
   assert(strstr(ctx, "CTX_SENTINEL_SHOULD_NOT_APPEAR") == NULL);
   assert(strstr(ctx, "# Relevant Context") == NULL);
   assert(strstr(ctx, "# Recall") == NULL);
   assert(strstr(ctx, "# Repos:") == NULL);

   free(ctx);
   teardown();
}

static void test_agent_context_budget_uses_context_window(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   assert(agent_exec_context_budget_chars(&ag) == AGENT_CONTEXT_BUDGET);

   ag.max_tokens = 1024;
   ag.middleware.context_window = 2048;
   assert(agent_exec_context_budget_chars(&ag) == 4096);

   ag.middleware.context_window = 32768;
   assert(agent_exec_context_budget_chars(&ag) > AGENT_CONTEXT_BUDGET);
}

/* An UNPINNED output reserve must not eat the window.
 *
 * middleware.context_window is frequently a deliberate POLICY ceiling below the
 * model's true capability (Claude bills a premium above 200k; Codex expects
 * <=272k), while model_max_output() reports the model's THEORETICAL maximum
 * (128k on current frontier models). Reserving the theoretical maximum out of a
 * policy-capped window collapsed the prompt budget: a 200k ceiling minus a 128k
 * reserve leaves 72k, a ~62% cut. Cap an unpinned reserve at a quarter of the
 * window; an operator who needs a long reply pins max_tokens, which is still
 * reserved in full. */
static void test_agent_context_budget_caps_unpinned_output_reserve(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   strcpy(ag.provider, "anthropic");
   strcpy(ag.model, "claude-opus-4-8");
   ag.middleware.context_window = 200000; /* deliberate policy ceiling */

   /* Unpinned: the reserve is min(model ceiling, window/4), so the prompt budget
    * is AT LEAST window - window/4 == 150000 tokens, and far above the 72000 a
    * full 128k reserve would have left. Asserted as an invariant rather than an
    * exact figure so the test does not pin whichever ceiling the catalog or the
    * heuristic currently reports for this model. */
   size_t unpinned = agent_exec_context_budget_chars(&ag);
   assert(unpinned >= (size_t)(200000 - 200000 / 4) * 4u);
   assert(unpinned > (size_t)72000 * 4u);

   /* An explicit operator cap is a real commitment and is reserved in full. */
   ag.max_tokens = 128000;
   assert(agent_exec_context_budget_chars(&ag) == (size_t)(200000 - 128000) * 4u);

   /* A model whose ceiling is genuinely small still reserves only that much,
    * rather than being inflated to window/4. */
   memset(&ag, 0, sizeof(ag));
   strcpy(ag.provider, "openai");
   strcpy(ag.model, "gpt-4"); /* small non-reasoning ceiling */
   ag.middleware.context_window = 1000000;
   size_t small = agent_exec_context_budget_chars(&ag);
   assert(small > (size_t)(1000000 - 1000000 / 4) * 4u);
}

int main(void)
{
   test_null_hint_produces_same_as_original();
   test_task_hint_prioritizes_relevant_memories();
   test_task_hint_respects_budget();
   test_task_hint_fills_all_sections();
   test_empty_db_with_task_hint();
   test_graph_boost_integration();
   test_task_hint_formats_xml_and_negative_context();
   test_classify_bug_fix();
   test_classify_refactor();
   test_classify_feature();
   test_classify_review();
   test_classify_test();
   test_classify_general();
   test_task_type_name_strings();
   test_agent_exec_context_truncates_large_prompt();
   test_agent_exec_context_ex_can_skip_kb();
   test_agent_context_budget_uses_context_window();
   test_agent_context_budget_caps_unpinned_output_reserve();
   printf("context_assembly: all tests passed\n");
   return 0;
}
