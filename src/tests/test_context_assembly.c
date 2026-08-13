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

/* The shape of the work comes from the ROLE, and the role alone.
 *
 * A keyword scan of the brief used to answer this. It read the prose again at
 * every context refresh, and the real roundtable panel prompt -- "must fail
 * closed", "must be fixed", alongside "Review" -- classified as a bug fix, so
 * every seat was handed execution-agent instructions and died without emitting
 * its verdict. The role is stated once and does not change mid-run.
 *
 * WHICH role maps to which shape is the module's list, pinned against the
 * module in server-go/modules/delegates/rolepolicy_test.go. What is asserted
 * HERE is that the answer reaches the instructions. */
static void test_the_role_decides_the_shape_of_the_work(void)
{
   assert(agent_task_type_for_role("review") == TASK_TYPE_REVIEW);
   assert(strstr(agent_exec_instructions(agent_task_type_for_role("review")),
                 "final message IS the deliverable"));

   assert(agent_task_type_for_role("code") != TASK_TYPE_REVIEW);
   assert(strstr(agent_exec_instructions(agent_task_type_for_role("code")), "execution agent"));

   /* No role is no delegate: neutral weighting, acting instructions. */
   assert(agent_task_type_for_role(NULL) == TASK_TYPE_GENERAL);
}

/* A reviewer's deliverable is its final message. The execution-agent
 * instruction "always invoke tools, never write as plain text" forbids exactly
 * that, and a reviewer given it spends its whole turn budget calling tools and
 * dies with "max turns exhausted without final response". */
static void test_review_instructions_do_not_forbid_a_final_answer(void)
{
   const char *review = agent_exec_instructions(TASK_TYPE_REVIEW);
   assert(strstr(review, "review agent"));
   assert(strstr(review, "final message IS the deliverable"));
   assert(!strstr(review, "Always invoke tools"));
   assert(!strstr(review, "Never write shell commands"));

   /* Acting agents keep their own instruction: they must call tools rather than
    * narrate shell commands. */
   for (task_type_t t = TASK_TYPE_GENERAL; t < TASK_TYPE_COUNT; t++)
   {
      if (t == TASK_TYPE_REVIEW)
         continue;
      const char *acting = agent_exec_instructions(t);
      assert(strstr(acting, "execution agent"));
      assert(strstr(acting, "Always invoke tools"));
   }
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

/* A pinned reserve at or above the whole window is a misconfiguration. The old
 * fallback silently advertised context_window/2 of PROMPT while still honouring
 * the oversized pinned reply, i.e. 400k of commitments against a 200k window.
 * Clamp the reserve instead so total commitments never exceed the window. */
static void test_agent_context_budget_clamps_oversized_pinned_output(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   strcpy(ag.provider, "anthropic");
   strcpy(ag.model, "claude-opus-4-8");
   ag.middleware.context_window = 200000;
   ag.max_tokens = 300000; /* larger than the whole window */

   /* Reserve clamps to window/2, so the prompt budget is the other half and
    * prompt + reserve == the window rather than exceeding it. */
   size_t budget = agent_exec_context_budget_chars(&ag);
   assert(budget == (size_t)(200000 - 200000 / 2) * 4u);

   /* Exactly equal to the window is the same misconfiguration. */
   ag.max_tokens = 200000;
   assert(agent_exec_context_budget_chars(&ag) == (size_t)(200000 - 200000 / 2) * 4u);
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

/* A restatement must not be assembled twice, on the PRODUCTION path.
 *
 * Read-time near-duplicate suppression was added to
 * memory_assemble_context_explain(), whose comment claims it "runs a candidate
 * scoring pass (identical to memory_assemble_context)". They are two separate
 * implementations, and the explain one has a single production caller (the
 * `memory improve` diagnostic) -- so the suppression never ran for `aimee memory
 * read` or the agent-runtime fallback context.
 *
 * Reproduced on a live deployment before this fix: two memories that are pure
 * word-order reorderings of each other -- identical token sets, so far above the
 * 0.85 lexical threshold -- BOTH appeared in the assembled context.
 *
 * The second assertion is the one that keeps the fix honest. Suppressing a
 * DISTINCT fact silently loses evidence, which is worse than admitting a
 * redundant line, so a memory that merely shares vocabulary must survive. */
static void test_restatements_are_suppressed_but_distinct_facts_survive(void)
{
   setup();
   memory_t m;

   memory_insert(TIER_L2, KIND_FACT, "release-a",
                 "The release checklist requires the changelog to be regenerated before any tag "
                 "is pushed to the origin remote",
                 0.9, "s1", &m);
   /* Same sentence, reordered: nothing new is said. */
   memory_insert(TIER_L2, KIND_FACT, "release-b",
                 "Before any tag is pushed to the origin remote, the release checklist requires "
                 "the changelog to be regenerated",
                 0.9, "s1", &m);
   /* Heavy vocabulary overlap, DIFFERENT claim. Must not be suppressed. */
   memory_insert(TIER_L2, KIND_FACT, "release-c",
                 "The release checklist forbids pushing any tag to the origin remote on a Friday",
                 0.9, "s1", &m);

   char *ctx = memory_assemble_context(NULL);
   assert(ctx != NULL);

   /* Count how many of the two restatements survived. */
   int a = strstr(ctx, "requires the changelog to be regenerated before any tag") != NULL;
   int b = strstr(ctx, "Before any tag is pushed to the origin remote, the release") != NULL;
   assert(a + b == 1); /* exactly one, not both */

   /* The distinct fact is still there. */
   assert(strstr(ctx, "on a Friday") != NULL);

   free(ctx);
   teardown();
   printf("  restatement suppressed, distinct fact kept\n");
}

int main(void)
{
   test_restatements_are_suppressed_but_distinct_facts_survive();
   test_null_hint_produces_same_as_original();
   test_task_hint_prioritizes_relevant_memories();
   test_task_hint_respects_budget();
   test_task_hint_fills_all_sections();
   test_empty_db_with_task_hint();
   test_graph_boost_integration();
   test_task_hint_formats_xml_and_negative_context();
   test_the_role_decides_the_shape_of_the_work();
   test_review_instructions_do_not_forbid_a_final_answer();
   test_task_type_name_strings();
   test_agent_exec_context_truncates_large_prompt();
   test_agent_exec_context_ex_can_skip_kb();
   test_agent_context_budget_uses_context_window();
   test_agent_context_budget_caps_unpinned_output_reserve();
   test_agent_context_budget_clamps_oversized_pinned_output();
   printf("context_assembly: all tests passed\n");
   return 0;
}
