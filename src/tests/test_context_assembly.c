#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "agent_exec.h"
#include "db1_client/db1.h"
#include "../modules/db2/c/db2.h"
#include "../modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"
#include "../modules/db2/c/lifecycle.h"
#include "../modules/db2/c/memory_query.h"
#include "../modules/db2/c/memory_relations.h"
#include "../modules/db2/c/memory_vectors.h"

static void setup(void)
{
   /* db1_init is idempotent: reuse a single in-memory db1 across all
    * tests. anti-pattern test cases clear the table at the start. */
   db2_test_shim_open();
   db2_set_ephemeral(1);
}

static void teardown(void)
{
   db2_test_shim_close();
}

static int count_text(const char *haystack, const char *needle)
{
   int count = 0;
   size_t n = strlen(needle);
   for (const char *p = haystack; n > 0 && (p = strstr(p, needle)) != NULL; p += n)
      count++;
   return count;
}

static void test_unit_cursor_drains_more_than_one_page(void)
{
   setup();
   memory_t parent = {0};
   assert(memory_insert(TIER_L2, KIND_FACT, "cursor-parent", "cursor parent fixture", 0.8,
                        "cursor-origin", &parent) == 0);

   char err[256] = "";
   aimee_pg_stmt_t *insert =
       aimee_pg_prepare(db2_conn(),
                        "INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text)"
                        " VALUES(?1,'chunk',?2,?3)",
                        err, sizeof(err));
   assert(insert != NULL);
   for (int i = 0; i < 130; i++)
   {
      char key[32], text[64];
      snprintf(key, sizeof(key), "unit-%03d", i);
      snprintf(text, sizeof(text), "cursor unit %03d", i);
      assert(aimee_pg_reset(insert) == 0);
      aimee_pg_bind_int64(insert, "?1", parent.id);
      aimee_pg_bind_text(insert, "?2", key);
      aimee_pg_bind_text(insert, "?3", text);
      assert(aimee_pg_step(insert, err, sizeof(err)) == AIMEE_PG_DONE);
   }
   aimee_pg_finalize(insert);

   aimee_pg_stmt_t *count = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*) FROM memory_units WHERE memory_id=?1", err, sizeof(err));
   assert(count != NULL);
   aimee_pg_bind_int64(count, "?1", parent.id);
   assert(aimee_pg_step(count, err, sizeof(err)) == AIMEE_PG_ROW);
   int expected = aimee_pg_column_int(count, 0);
   aimee_pg_finalize(count);
   assert(expected >= 130);

   int64_t after = 0;
   int total = 0;
   int has_more = 0;
   do
   {
      int64_t ids[17];
      int n = db2_memory_unit_list_ids_after(parent.id, after, ids, 17, &has_more);
      assert(n > 0 && n <= 17);
      for (int i = 0; i < n; i++)
      {
         assert(ids[i] > after);
         after = ids[i];
      }
      total += n;
   } while (has_more);
   assert(total == expected);

   /* Seed every synthetic point, including more than the old reclamation
    * buffer, without a parent FK that could hide a missed delete. */
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO memory_embeddings(point_id)"
            " SELECT id+%lld FROM memory_units WHERE memory_id=%lld",
            (long long)PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET, (long long)parent.id);
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
   snprintf(sql, sizeof(sql), "INSERT INTO memory_embeddings(point_id) VALUES(%lld)",
            (long long)parent.id);
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
   snprintf(sql, sizeof(sql),
            "INSERT INTO vector_index_ops(point_id,memory_id)"
            " SELECT id+%lld,NULL FROM memory_units WHERE memory_id=%lld",
            (long long)PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET, (long long)parent.id);
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
   snprintf(sql, sizeof(sql), "INSERT INTO vector_index_ops(point_id,memory_id) VALUES(%lld,NULL)",
            (long long)parent.id);
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);

   assert(memory_delete(parent.id) == 0);
   aimee_pg_stmt_t *remaining = aimee_pg_prepare(db2_conn(),
                                                 "SELECT (SELECT COUNT(*) FROM memory_embeddings)"
                                                 "     + (SELECT COUNT(*) FROM vector_index_ops)",
                                                 err, sizeof(err));
   assert(remaining != NULL);
   assert(aimee_pg_step(remaining, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(remaining, 0) == 0);
   aimee_pg_finalize(remaining);

   teardown();
}

static void test_origin_diversity_and_labels_reach_production(void)
{
   setup();
   memory_t m = {0};
   assert(memory_insert(TIER_L2, KIND_FACT, "origin-alpha-one",
                        "ORIGIN_ALPHA_ONE quota evidence about release signatures", 0.8,
                        "origin-alpha", &m) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "origin-alpha-two",
                        "ORIGIN_ALPHA_TWO quota evidence about deployment regions", 0.8,
                        "origin-alpha", &m) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "origin-alpha-three",
                        "ORIGIN_ALPHA_THREE quota evidence about database retention", 0.8,
                        "origin-alpha", &m) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "origin-beta-one",
                        "ORIGIN_BETA_ONE quota evidence from an independent session", 0.2,
                        "origin&beta", &m) == 0);

   context_assemble_explain_entry_t explain[16];
   context_budget_metrics_t metrics;
   int explain_count = 0;
   char *ctx =
       memory_assemble_context_explain("quota evidence", explain, &explain_count, 16, &metrics);
   assert(ctx != NULL);
   assert(count_text(ctx, "origin_session=\"origin-alpha\"") == 2);
   assert(count_text(ctx, "origin_session=\"origin&amp;beta\"") == 1);
   assert(metrics.deferred_for_origin_quota >= 2);
   int quota_rejected = 0;
   for (int i = 0; i < explain_count; i++)
      if (strcmp(explain[i].rejection_reason, "origin_quota_cap") == 0)
         quota_rejected++;
   assert(quota_rejected >= 1);
   free(ctx);

   teardown();
}

static void test_withheld_memories_cannot_reenter_production_reads(void)
{
   setup();
   memory_t active = {0}, archived = {0}, suppressed = {0};
   assert(memory_insert(TIER_L2, KIND_FACT, "negative-active", "NEGATIVE_ACTIVE_VISIBLE", 0.9,
                        "origin-a", &active) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "negative-archived", "NEGATIVE_ARCHIVED_HIDDEN", 0.9,
                        "origin-b", &archived) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "negative-suppressed", "NEGATIVE_SUPPRESSED_HIDDEN",
                        0.9, "origin-c", &suppressed) == 0);
   assert(memory_transition_lifecycle(archived.id, MEMORY_LIFECYCLE_STATE_ARCHIVED,
                                      "negative retrieval fixture") == 0);
   assert(memory_activation_policy_set(suppressed.id, 2, 1, 0, 1) == 0);

   /* Envelope assembly drives the real candidate query and emitter. */
   char *ctx = memory_assemble_context("negative reachability sentinel");
   assert(ctx != NULL);
   assert(strstr(ctx, "NEGATIVE_ACTIVE_VISIBLE") != NULL);
   assert(strstr(ctx, "NEGATIVE_ARCHIVED_HIDDEN") == NULL);
   assert(strstr(ctx, "NEGATIVE_SUPPRESSED_HIDDEN") == NULL);
   free(ctx);

   /* Graph recall is a separate production lane and must apply the same
    * negative predicate rather than trusting envelope filtering downstream. */
   db2_memory_relation_insert(active.id, "neggraph-active", "rel", "target", "active relation");
   db2_memory_relation_insert(archived.id, "neggraph-archived", "rel", "target",
                              "archived relation");
   db2_memory_relation_insert(suppressed.id, "neggraph-suppressed", "rel", "target",
                              "suppressed relation");
   memory_relation_t relations[8];
   assert(memory_search_graph("neggraph-active", 8, relations, 8) == 1);
   assert(memory_search_graph("neggraph-archived", 8, relations, 8) == 0);
   assert(memory_search_graph("neggraph-suppressed", 8, relations, 8) == 0);

   /* Episode cards bypass the ordinary fact candidate pool, so exercise that
    * production query too. */
   db2_memory_unit_episode_card_insert(archived.id, "card-a", "ARCHIVED_CARD_HIDDEN");
   db2_memory_unit_episode_card_insert(suppressed.id, "card-s", "SUPPRESSED_CARD_HIDDEN");
   ctx = memory_assemble_context("what happened in the session overview?");
   assert(ctx != NULL);
   assert(strstr(ctx, "ARCHIVED_CARD_HIDDEN") == NULL);
   assert(strstr(ctx, "SUPPRESSED_CARD_HIDDEN") == NULL);
   free(ctx);

   teardown();
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
   test_unit_cursor_drains_more_than_one_page();
   test_origin_diversity_and_labels_reach_production();
   test_withheld_memories_cannot_reenter_production_reads();
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
