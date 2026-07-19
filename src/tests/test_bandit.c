/* test_bandit.c — unit tests for the contextual bandit substrate.
 *
 * Tests:
 *   1. bandit_arm_register: write a policy_arm artifact and verify it exists.
 *   2. bandit_sample_disabled: returns -1 when command empty.
 *   3. bandit_reward_closed: close decision and update stats.
 *   4. config_bandit_defaults: config defaults are sane.
 *   5. bandit_explore_stats: window query counts decisions split by is_exploration.
 *   6. bandit_replay_evidence: writes a benchmark_trace artifact from replay JSON.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "db2_test_shim.h"
#include "../kb_bandit.h"
#include "../kb_bandit_registry.h"
#include "../db2/bandit.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "config.h"
#include "config_learning.h"

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* ---- 1. bandit_arm_register ---- */
static void test_bandit_arm_register(void)
{
   open_db();

   const char *variant = "{\"fusion_alpha\":0.5}";
   const char *reward_schema =
       "{\"components\":[{\"name\":\"accept_rate\",\"weight\":1.0,\"source\":\"audit_outcome\"}]}";

   int rc = kb_bandit_arm_register("test_dp", "arm_alpha", variant, reward_schema);
   assert(rc == 0);

   /* Second call must not fail (update path). */
   rc = kb_bandit_arm_register("test_dp", "arm_alpha", variant, reward_schema);
   assert(rc == 0);

   close_db();
   printf("  bandit_arm_register: ok\n");
}

/* ---- 2. bandit_sample_disabled ---- */
static void test_bandit_sample_disabled(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* bandit_optimize_command empty = disabled */

   char arm_ids[2][KB_BANDIT_MAX_ARM_ID];
   strncpy(arm_ids[0], "arm_a", KB_BANDIT_MAX_ARM_ID - 1);
   strncpy(arm_ids[1], "arm_b", KB_BANDIT_MAX_ARM_ID - 1);

   char decision_id[KB_BANDIT_MAX_DECISION] = "";
   int rc = kb_bandit_sample(&cfg, "test_dp", NULL, (const char(*)[KB_BANDIT_MAX_ARM_ID])arm_ids, 2,
                             decision_id);
   assert(rc == -1);

   printf("  bandit_sample_disabled: ok\n");
}

/* ---- 3. bandit_reward_closed ---- */
static void test_bandit_reward_closed(void)
{
   open_db();

   /* Insert a synthetic decision directly. */
   const char *fake_id = "test-decision-uuid-0001234567890ab";
   int rc = db2_bandit_decision_insert(fake_id, "test_dp2", "arm_beta", "", 0.8, 0);
   assert(rc == 0);

   /* Close with a reward. */
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   rc = kb_bandit_reward(&cfg, "test_dp2", fake_id, "arm_beta", 1.0);
   assert(rc == 0);

   /* Verify arm stats were updated. */
   db2_bandit_arm_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   rc = db2_bandit_arm_stats_read("test_dp2", "arm_beta", &stats);
   assert(rc == 0);
   /* After one reward=1.0 update: alpha = 1(prior) + 1.0 = 2.0, beta = 1(prior) + 0.0 = 1.0 */
   assert(stats.posterior_alpha >= 1.9 && stats.posterior_alpha <= 2.1);
   assert(stats.posterior_beta >= 0.9 && stats.posterior_beta <= 1.1);

   close_db();
   printf("  bandit_reward_closed: ok\n");
}

/* ---- 4. config_bandit_defaults ---- */
static void test_config_bandit_defaults(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_bandit_settings(&cfg, NULL);

   assert(cfg.bandit_optimize_command[0] == '\0');
   assert(cfg.bandit_exploration_fraction >= 0.04 && cfg.bandit_exploration_fraction <= 0.06);
   assert(cfg.bandit_ipw_weight_cap >= 9.9 && cfg.bandit_ipw_weight_cap <= 10.1);
   assert(cfg.bandit_exploration_window_seconds == 7 * 24 * 3600);

   printf("  config_bandit_defaults: ok\n");
}

/* ---- 5. bandit_explore_stats ---- */
static void test_bandit_explore_stats(void)
{
   open_db();

   /* Insert a mix of exploration and exploit decisions for two decision_points. */
   for (int i = 0; i < 12; i++)
   {
      char id[64];
      snprintf(id, sizeof(id), "test-explore-stats-%02d", i);
      int is_explore = (i < 3) ? 1 : 0; /* 3 of 12 = 25% */
      int rc = db2_bandit_decision_insert(id, "test_dp_stats", "arm_x", "", 0.5, is_explore);
      assert(rc == 0);
   }

   /* Sibling decision point — must not leak into the count. */
   int rc = db2_bandit_decision_insert("test-other-dp-01", "other_dp", "arm_y", "", 0.5, 1);
   assert(rc == 0);

   long long n_explore = 0, n_total = 0;
   rc = db2_bandit_explore_stats("test_dp_stats", 0, &n_explore, &n_total);
   assert(rc == 0);
   assert(n_total == 12);
   assert(n_explore == 3);

   /* Other decision point sees only its own row. */
   n_explore = n_total = 0;
   rc = db2_bandit_explore_stats("other_dp", 0, &n_explore, &n_total);
   assert(rc == 0);
   assert(n_total == 1);
   assert(n_explore == 1);

   close_db();
   printf("  bandit_explore_stats: ok\n");
}

/* ---- decision-point registry ---- */
static void test_bandit_registry(void)
{
   /* At least the live retrieval-limit point is registered. */
   assert(kb_bandit_registry_count() >= 1);

   const kb_bandit_decision_point_t *dp = kb_bandit_registry_get("kb_memory_retrieval_limit");
   assert(dp != NULL);
   assert(strcmp(dp->id, "kb_memory_retrieval_limit") == 0);
   assert(strcmp(dp->status, "live") == 0);
   assert(strcmp(dp->reward_fn, "recall_sufficiency_v1") == 0);
   assert(dp->n_arms == 2);
   assert(strcmp(dp->arms[0], "10") == 0);
   assert(strcmp(dp->arms[1], "20") == 0);

   /* kb_fusion_mode is also registered (KB-search fusion strategy). */
   const kb_bandit_decision_point_t *fm = kb_bandit_registry_get("kb_fusion_mode");
   assert(fm != NULL);
   assert(fm->n_arms == 3);
   assert(strcmp(fm->arms[0], "rrf") == 0);
   assert(strcmp(fm->status, "live") == 0);

   /* delegate_routing (server-side decision point reached via the kb bandit). */
   const kb_bandit_decision_point_t *dr = kb_bandit_registry_get("delegate_routing");
   assert(dr != NULL);
   assert(dr->n_arms == 2);
   assert(strcmp(dr->arms[0], "cheapest") == 0);
   assert(strcmp(dr->arms[1], "premium") == 0);

   const kb_bandit_decision_point_t *bs = kb_bandit_registry_get("briefing_style");
   assert(bs != NULL);
   assert(bs->n_arms == 2);
   assert(strcmp(bs->arms[0], "compact") == 0);
   assert(strcmp(bs->arms[1], "evidence_heavy") == 0);
   assert(strcmp(bs->status, "static") == 0);

   const kb_bandit_decision_point_t *gs = kb_bandit_registry_get("guardrail_strictness");
   assert(gs != NULL);
   assert(gs->n_arms == 2);
   assert(strcmp(gs->arms[0], "balanced") == 0);
   assert(strcmp(gs->arms[1], "strict") == 0);
   assert(strcmp(gs->status, "static") == 0);

   /* Unknown id -> NULL; index access is bounds-checked. */
   assert(kb_bandit_registry_get("nope") == NULL);
   assert(kb_bandit_registry_get(NULL) == NULL);
   assert(kb_bandit_registry_at(-1) == NULL);
   assert(kb_bandit_registry_at(kb_bandit_registry_count()) == NULL);
   assert(kb_bandit_registry_at(0) != NULL);

   printf("  bandit_registry: ok\n");
}

/* ---- recall-sufficiency reward (pure) ---- */
static void test_bandit_recall_reward(void)
{
   /* Empty recall is bad at any limit. */
   assert(kb_bandit_recall_sufficiency_reward(0, 10) == 0.0);
   assert(kb_bandit_recall_sufficiency_reward(0, 20) == 0.0);

   /* Sufficient, non-truncated recall scores 1.0 — and is not biased toward the
    * larger arm: 8 results satisfy both the 10 and 20 arms. */
   assert(kb_bandit_recall_sufficiency_reward(8, 10) == 1.0);
   assert(kb_bandit_recall_sufficiency_reward(8, 20) == 1.0);

   /* Hitting the cap is a truncation signal (a larger limit might help). */
   assert(kb_bandit_recall_sufficiency_reward(10, 10) == 0.5);
   assert(kb_bandit_recall_sufficiency_reward(20, 20) == 0.5);

   /* 10 results: truncated for the 10-arm, sufficient for the 20-arm. */
   assert(kb_bandit_recall_sufficiency_reward(10, 20) == 1.0);

   printf("  bandit_recall_reward: ok\n");
}

/* ---- 7. decision_points_list / arms_list ---- */
static void test_bandit_enumeration(void)
{
   open_db();

   /* Two points, the first with two arms; a sibling point must not leak in. */
   assert(db2_bandit_decision_insert("enum-a-1", "kb_memory_retrieval_limit", "10", "", 0.5, 0) ==
          0);
   assert(db2_bandit_decision_insert("enum-a-2", "kb_memory_retrieval_limit", "20", "", 0.5, 0) ==
          0);
   assert(db2_bandit_decision_insert("enum-a-3", "kb_memory_retrieval_limit", "10", "", 0.5, 0) ==
          0);
   assert(db2_bandit_decision_insert("enum-b-1", "other_dp", "x", "", 0.5, 0) == 0);

   char buf[2048];

   /* Points list: both points present, none invented. */
   assert(db2_bandit_decision_points_list(buf, sizeof(buf)) == 0);
   assert(strstr(buf, "\"kb_memory_retrieval_limit\"") != NULL);
   assert(strstr(buf, "\"other_dp\"") != NULL);
   assert(strstr(buf, "kb_fusion_mode") == NULL);

   /* Arms list: distinct arms for the point, scoped (no sibling-point arm). */
   assert(db2_bandit_arms_list("kb_memory_retrieval_limit", buf, sizeof(buf)) == 0);
   assert(strstr(buf, "\"10\"") != NULL);
   assert(strstr(buf, "\"20\"") != NULL);
   assert(strstr(buf, "\"x\"") == NULL);

   /* Unknown point: empty array, not an error. */
   assert(db2_bandit_arms_list("no_such_point", buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "[]") == 0);

   close_db();
   printf("  bandit_enumeration: ok\n");
}

/* ---- 6. bandit_replay_evidence ---- */
static void test_bandit_replay_evidence(void)
{
   open_db();

   const char *ipw_json =
       "{\"estimator\":\"ipw\",\"status\":\"ok\",\"target_arm\":\"arm_b\","
       "\"v_hat\":1.25,\"ci_low\":0.9,\"ci_high\":1.6,\"n_matched\":4,\"n_total\":8}";

   char artifact_id[64] = "";
   int rc = kb_bandit_record_replay_evidence("kb_fusion_mode", ipw_json, artifact_id,
                                             sizeof(artifact_id));
   assert(rc == 0);
   assert(artifact_id[0] != '\0');

   /* Verify the artifact landed at kind=benchmark_trace. */
   void *conn = db2_conn();
   assert(conn);
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT kind, scope_kind, scope_id FROM artifacts WHERE id = ?1", err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", artifact_id);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   const char *kind = aimee_pg_column_text(st, 0);
   const char *scope_kind = aimee_pg_column_text(st, 1);
   const char *scope_id = aimee_pg_column_text(st, 2);
   assert(kind && strcmp(kind, "benchmark_trace") == 0);
   assert(scope_kind && strcmp(scope_kind, "bandit_replay") == 0);
   assert(scope_id && strcmp(scope_id, "kb_fusion_mode") == 0);
   aimee_pg_finalize(st);

   close_db();
   printf("  bandit_replay_evidence: ok\n");
}

/* ---- main ---- */
int main(void)
{
   printf("bandit:\n");

   test_bandit_arm_register();
   test_bandit_sample_disabled();
   test_bandit_reward_closed();
   test_config_bandit_defaults();
   test_bandit_explore_stats();
   test_bandit_registry();
   test_bandit_recall_reward();
   test_bandit_enumeration();
   test_bandit_replay_evidence();

   printf("All bandit tests passed.\n");
   return 0;
}
