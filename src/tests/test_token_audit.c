#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h" /* MAX_PATH_LEN, pulled in before agent_types.h */
#include "agent_exec.h"
#include "db1.h"
#include <sqlite3.h>

/* Private to src/db1/, but this test adjusts timestamps directly. */
extern sqlite3 *db1_conn(void);

static void seed_rows(void)
{
   db1_token_audit_row_t row1 = {
       .session_id = "s1",
       .project_name = "proj",
       .tool_name = "tool-a",
       .role = "implement",
       .model = "gpt-4o",
       .prompt_tokens = 100,
       .completion_tokens = 40,
       .cache_write_tokens = 20,
       .cache_read_tokens = 10,
       .estimated_cost_usd = 0.20,
   };
   db1_token_audit_row_t row2 = {
       .session_id = "s2",
       .project_name = "proj",
       .tool_name = "tool-b",
       .role = "review",
       .model = "gpt-4o",
       .prompt_tokens = 30,
       .completion_tokens = 10,
       .cache_write_tokens = 5,
       .cache_read_tokens = 1,
       .estimated_cost_usd = 0.05,
   };
   db1_token_audit_row_t row3 = {
       .session_id = "s3",
       .project_name = "proj",
       .tool_name = "tool-a",
       .role = "implement",
       .model = "gpt-4o",
       .prompt_tokens = 7,
       .completion_tokens = 3,
       .cache_write_tokens = 1,
       .cache_read_tokens = 2,
       .estimated_cost_usd = 0.01,
   };

   assert(db1_token_audit_insert(&row1) == 0);
   assert(db1_token_audit_insert(&row2) == 0);
   assert(db1_token_audit_insert(&row3) == 0);
   assert(sqlite3_exec(db1_conn(),
                       "UPDATE token_audit"
                       " SET created_at = datetime('now', '-30 hours')"
                       " WHERE session_id = 's3'",
                       NULL, NULL, NULL) == SQLITE_OK);
}

static void test_totals_and_filters(void)
{
   db1_token_audit_totals_t totals;
   assert(db1_token_audit_totals(0, &totals) == 0);
   assert(totals.total_calls == 3);
   assert(totals.prompt_tokens == 137);
   assert(totals.completion_tokens == 53);
   assert(totals.cache_write_tokens == 26);
   assert(totals.cache_read_tokens == 13);
   assert(totals.estimated_cost_usd > 0.25 && totals.estimated_cost_usd < 0.27);

   assert(db1_token_audit_totals(24, &totals) == 0);
   assert(totals.total_calls == 2);
   assert(totals.prompt_tokens == 130);
   assert(totals.completion_tokens == 50);
   assert(totals.cache_write_tokens == 25);
   assert(totals.cache_read_tokens == 11);
   assert(totals.estimated_cost_usd > 0.24 && totals.estimated_cost_usd < 0.26);
}

static void test_grouped_views(void)
{
   db1_token_audit_role_summary_t roles[4];
   int role_count = db1_token_audit_by_role(0, roles, 4);
   assert(role_count == 2);
   assert(strcmp(roles[0].role, "implement") == 0);
   assert(roles[0].calls == 2);
   assert(roles[0].prompt_tokens == 107);
   assert(roles[0].completion_tokens == 43);
   assert(strcmp(roles[1].role, "review") == 0);
   assert(roles[1].calls == 1);

   db1_token_audit_tool_summary_t tools[4];
   int tool_count = db1_token_audit_by_tool(24, tools, 4);
   assert(tool_count == 2);
   assert(strcmp(tools[0].tool_name, "tool-a") == 0);
   assert(tools[0].calls == 1);
   assert(tools[0].prompt_tokens == 100);
   assert(tools[0].completion_tokens == 40);
   assert(strcmp(tools[1].tool_name, "tool-b") == 0);
}

static void test_cost_for_delegation(void)
{
   /* Add two delegate-tagged rows under one delegation_id and an
    * unrelated row under a different one. Sum returns only the
    * matching rows; unknown delegation returns 0. */
   db1_token_audit_row_t child_a = {
       .session_id = "s-parent",
       .delegation_id = "deleg-100",
       .tool_name = "code",
       .role = "code",
       .model = "gpt",
       .prompt_tokens = 1,
       .completion_tokens = 1,
       .estimated_cost_usd = 0.10,
   };
   db1_token_audit_row_t child_b = {
       .session_id = "s-parent",
       .delegation_id = "deleg-100",
       .tool_name = "code",
       .role = "code",
       .model = "gpt",
       .prompt_tokens = 1,
       .completion_tokens = 1,
       .estimated_cost_usd = 0.07,
   };
   db1_token_audit_row_t other = {
       .session_id = "s-parent",
       .delegation_id = "deleg-999",
       .tool_name = "code",
       .role = "code",
       .model = "gpt",
       .prompt_tokens = 1,
       .completion_tokens = 1,
       .estimated_cost_usd = 0.42,
   };
   assert(db1_token_audit_insert(&child_a) == 0);
   assert(db1_token_audit_insert(&child_b) == 0);
   assert(db1_token_audit_insert(&other) == 0);

   double sum = db1_token_audit_cost_for_delegation("deleg-100");
   assert(sum > 0.169 && sum < 0.171);

   /* Unknown delegation returns 0.0 — base case for the cost-fold path. */
   assert(db1_token_audit_cost_for_delegation("does-not-exist") == 0.0);
   /* Empty / NULL delegation_id treated as unknown (not a wildcard). */
   assert(db1_token_audit_cost_for_delegation("") == 0.0);
   assert(db1_token_audit_cost_for_delegation(NULL) == 0.0);
}

static void test_by_model_relabels_empty(void)
{
   /* Insert one row with a real model and one legacy row with an empty
    * model. by_model must surface BOTH — the empty one relabelled as
    * "(unattributed)" rather than dropped — so historical spend stays
    * visible once the model column starts being populated. */
   db1_token_audit_row_t with_model = {
       .session_id = "bm1",
       .tool_name = "claude-opus-4",
       .role = "implement",
       .model = "claude-opus-4",
       .prompt_tokens = 50,
       .completion_tokens = 20,
       .estimated_cost_usd = 0.30,
   };
   db1_token_audit_row_t legacy_empty = {
       .session_id = "bm2",
       .tool_name = "legacy-agent",
       .role = "implement",
       .model = "", /* legacy row written before model was tracked */
       .prompt_tokens = 11,
       .completion_tokens = 4,
       .estimated_cost_usd = 0.02,
   };
   assert(db1_token_audit_insert(&with_model) == 0);
   assert(db1_token_audit_insert(&legacy_empty) == 0);

   db1_token_audit_model_summary_t models[16];
   int n = db1_token_audit_by_model(0, models, 16);
   assert(n > 0);

   int saw_real = 0, saw_unattributed = 0;
   for (int i = 0; i < n; i++)
   {
      assert(models[i].model[0] != '\0'); /* no label is ever empty */
      if (strcmp(models[i].model, "claude-opus-4") == 0)
         saw_real = 1;
      if (strcmp(models[i].model, "(unattributed)") == 0)
      {
         saw_unattributed = 1;
         assert(models[i].prompt_tokens >= 11);
      }
   }
   assert(saw_real);
   assert(saw_unattributed);
}

static int count_where(const char *col, const char *val)
{
   char sql[160];
   snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM token_audit WHERE %s = ?", col);
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db1_conn(), sql, -1, &st, NULL) == SQLITE_OK);
   sqlite3_bind_text(st, 1, val, -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

static void test_by_source_groups_and_relabels(void)
{
   /* Rows tagged with a turn origin group by source; an untagged (legacy)
    * row surfaces as "(unattributed)" rather than being dropped. */
   db1_token_audit_row_t ingress = {
       .session_id = "src1",
       .tool_name = "gpt-4o",
       .role = "implement",
       .model = "gpt-4o",
       .source = "openai-ingress",
       .prompt_tokens = 40,
       .completion_tokens = 10,
       .estimated_cost_usd = 0.05,
   };
   db1_token_audit_row_t untagged = {
       .session_id = "src2",
       .tool_name = "gpt-4o",
       .role = "implement",
       .model = "gpt-4o",
       .source = "", /* legacy row before source was tracked */
       .prompt_tokens = 5,
       .completion_tokens = 2,
       .estimated_cost_usd = 0.01,
   };
   assert(db1_token_audit_insert(&ingress) == 0);
   assert(db1_token_audit_insert(&untagged) == 0);

   db1_token_audit_source_summary_t sources[16];
   int n = db1_token_audit_by_source(0, sources, 16);
   assert(n > 0);
   int saw_ingress = 0, saw_unattributed = 0;
   for (int i = 0; i < n; i++)
   {
      assert(sources[i].source[0] != '\0'); /* no label is ever empty */
      if (strcmp(sources[i].source, "openai-ingress") == 0)
         saw_ingress = 1;
      if (strcmp(sources[i].source, "(unattributed)") == 0)
         saw_unattributed = 1;
   }
   assert(saw_ingress);
   assert(saw_unattributed);
}

static void test_agent_log_call_records_served_model(void)
{
   /* The broken path the model-attribution fix targets: an agent whose
    * name differs from its served model. agent_log_call must record the
    * MODEL in token_audit.model (and key cost off it), not the agent name. */
   agent_result_t r;
   memset(&r, 0, sizeof(r));
   snprintf(r.agent_name, sizeof(r.agent_name), "codex");
   snprintf(r.model, sizeof(r.model), "gpt-4o"); /* priced, so cost must be > 0 */
   r.prompt_tokens = 1000;
   r.completion_tokens = 500;
   r.success = 1;
   agent_log_call(&r, "implement");

   /* model column carries the served model, with non-zero cost keyed off it. */
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db1_conn(),
                             "SELECT COALESCE(SUM(estimated_cost_usd), 0) FROM token_audit"
                             " WHERE model = 'gpt-4o'",
                             -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   double cost = sqlite3_column_double(st, 0);
   sqlite3_finalize(st);
   assert(cost > 0.0); /* would be 0 if cost were keyed off "codex" */

   /* The agent name must NOT leak into the model column... */
   assert(count_where("model", "codex") == 0);
   /* ...but it is still recorded as the tool/agent identity. */
   assert(count_where("tool_name", "codex") >= 1);
   /* Internal agent execution is tagged with the "agent" source. */
   assert(count_where("source", "agent") >= 1);
}

static void test_record_token_audit_ingress_source(void)
{
   /* The shared helper the ingress handlers use: it records one cost row tagged
    * with the given source, billed to the served model, and (unlike
    * agent_log_call) writes no agent_log row. */
   agent_result_t r;
   memset(&r, 0, sizeof(r));
   snprintf(r.agent_name, sizeof(r.agent_name), "primary");
   snprintf(r.model, sizeof(r.model), "claude-3-5-sonnet");                 /* served */
   snprintf(r.requested_model, sizeof(r.requested_model), "claude-opus-4"); /* client asked */
   snprintf(r.stop_reason, sizeof(r.stop_reason), "end_turn");
   r.prompt_tokens = 2000;
   r.completion_tokens = 1000;
   r.success = 1;
   agent_record_token_audit(&r, "", "openai-ingress");

   /* requested model recorded separately from served; stop_reason captured;
    * usage_kind defaults to "realized". */
   assert(count_where("requested_model", "claude-opus-4") == 1);
   assert(count_where("stop_reason", "end_turn") == 1);
   assert(count_where("usage_kind", "realized") >= 1);

   /* Exactly one row for this served model under the ingress source. */
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db1_conn(),
                             "SELECT COUNT(*), COALESCE(SUM(estimated_cost_usd), 0)"
                             " FROM token_audit"
                             " WHERE source = 'openai-ingress' AND model = 'claude-3-5-sonnet'",
                             -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int cnt = sqlite3_column_int(st, 0);
   double cost = sqlite3_column_double(st, 1);
   sqlite3_finalize(st);
   assert(cnt == 1);
   assert(cost > 0.0); /* billed against the served model, not $0 */
}

static void test_ingress_source_override(void)
{
   /* A thread-scoped ingress source overrides the caller's source on the row,
    * so /v1/runs (which logs "agent" from inside the run loop) is tagged as
    * ingress without a second row. */
   agent_set_ingress_source("openai-ingress");
   agent_result_t r;
   memset(&r, 0, sizeof(r));
   snprintf(r.agent_name, sizeof(r.agent_name), "runbot");
   snprintf(r.model, sizeof(r.model), "gpt-4o");
   r.prompt_tokens = 100;
   r.completion_tokens = 50;
   agent_record_token_audit(&r, "execute", "agent"); /* caller passes "agent" */
   agent_set_ingress_source("");                     /* clear for later tests */

   assert(count_where("tool_name", "runbot") == 1);
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db1_conn(),
                             "SELECT source FROM token_audit WHERE tool_name = 'runbot'", -1, &st,
                             NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(st, 0), "openai-ingress") == 0);
   sqlite3_finalize(st);
}

static long long insert_agent_log(const char *agent, const char *role)
{
   db1_agent_log_insert_row_t log = {
       .agent_name = agent,
       .role = role,
       .prompt_tokens = 10,
       .completion_tokens = 5,
       .success = 1,
       .confidence = -1,
   };
   long long id = db1_agent_log_insert(&log);
   assert(id > 0);
   return id;
}

static void test_agent_stats_join_is_1to1(void)
{
   /* Agent stats join token_audit by agent_log_id (1:1), so: (a) ingress rows
    * with no agent_log row do not inflate stats, and (b) two calls to the same
    * agent do not multiply cost via the old (agent_name, role) cartesian join. */
   long long id1 = insert_agent_log("statbot", "execute");
   long long id2 = insert_agent_log("statbot", "execute");

   db1_token_audit_row_t a = {.tool_name = "statbot",
                              .role = "execute",
                              .model = "gpt-4o",
                              .source = "agent",
                              .agent_log_id = id1,
                              .estimated_cost_usd = 0.10};
   db1_token_audit_row_t b = {.tool_name = "statbot",
                              .role = "execute",
                              .model = "gpt-4o",
                              .source = "agent",
                              .agent_log_id = id2,
                              .estimated_cost_usd = 0.20};
   /* An ingress row sharing the agent name, with no agent_log link. */
   db1_token_audit_row_t ingress = {.tool_name = "statbot",
                                    .role = "execute",
                                    .model = "gpt-4o",
                                    .source = "openai-ingress",
                                    .agent_log_id = 0,
                                    .estimated_cost_usd = 9.99};
   assert(db1_token_audit_insert(&a) == 0);
   assert(db1_token_audit_insert(&b) == 0);
   assert(db1_token_audit_insert(&ingress) == 0);

   db1_agent_log_agent_stats_t stats[4];
   int n = db1_agent_log_agent_stats("statbot", stats, 4);
   assert(n == 1);
   /* Exactly $0.10 + $0.20 = $0.30 — not multiplied (would be $0.60 under the
    * old cartesian join) and not inflated by the $9.99 ingress row. */
   assert(stats[0].total_estimated_cost_usd > 0.29 && stats[0].total_estimated_cost_usd < 0.31);
}

static void test_spend_breakdown_excludes_avoided(void)
{
   /* The §7 spend authority groups cost by usage_kind. Measure deltas against a
    * baseline so this is independent of the rows other tests already inserted. */
   db1_token_audit_spend_t before;
   assert(db1_token_audit_spend_breakdown(0, &before) == 0);

   db1_token_audit_row_t realized = {.session_id = "sb-real",
                                     .tool_name = "gpt-4o",
                                     .role = "implement",
                                     .model = "gpt-4o",
                                     .usage_kind = "realized",
                                     .estimated_cost_usd = 0.25};
   db1_token_audit_row_t estimated = {.session_id = "sb-est",
                                      .tool_name = "gpt-4o",
                                      .role = "implement",
                                      .model = "gpt-4o",
                                      .usage_kind = "estimated",
                                      .estimated_cost_usd = 0.50};
   db1_token_audit_row_t avoided = {.session_id = "sb-avoid",
                                    .tool_name = "gpt-4o",
                                    .role = "implement",
                                    .model = "gpt-4o",
                                    .usage_kind = "avoided",
                                    .estimated_cost_usd = 1.00};
   assert(db1_token_audit_insert(&realized) == 0);
   assert(db1_token_audit_insert(&estimated) == 0);
   assert(db1_token_audit_insert(&avoided) == 0);

   db1_token_audit_spend_t after;
   assert(db1_token_audit_spend_breakdown(0, &after) == 0);

   double d_real = after.realized_cost_usd - before.realized_cost_usd;
   double d_est = after.estimated_cost_usd - before.estimated_cost_usd;
   double d_avoid = after.avoided_cost_usd - before.avoided_cost_usd;
   double d_total = after.spend_cost_usd - before.spend_cost_usd;
   assert(d_real > 0.249 && d_real < 0.251);
   assert(d_est > 0.499 && d_est < 0.501);
   assert(d_avoid > 0.999 && d_avoid < 1.001);
   /* Billable total moved by realized + estimated only — avoided is excluded. */
   assert(d_total > 0.749 && d_total < 0.751);
}

static void test_dashboard_rows(void)
{
   db1_token_audit_dashboard_row_t rows[4];
   int count = db1_token_audit_list_dashboard(rows, 4);
   assert(count == 2);
   assert(strcmp(rows[0].tool_name, "tool-a") == 0);
   assert(strcmp(rows[0].role, "implement") == 0);
   assert(rows[0].call_count == 2);
   assert(rows[0].cache_write_tokens == 21);
   assert(rows[0].cache_read_tokens == 12);
   assert(rows[0].last_seen[0] != '\0');
}

int main(void)
{
   assert(db1_init(":memory:") == 0);
   seed_rows();
   test_totals_and_filters();
   test_grouped_views();
   test_dashboard_rows();
   /* These run after the count-sensitive tests because they insert
    * additional rows that would skew the totals/grouped/dashboard counts. */
   test_by_model_relabels_empty();
   test_by_source_groups_and_relabels();
   test_agent_log_call_records_served_model();
   test_record_token_audit_ingress_source();
   test_ingress_source_override();
   test_agent_stats_join_is_1to1();
   test_cost_for_delegation();
   test_spend_breakdown_excludes_avoided();
   db1_shutdown();
   printf("test_token_audit: ok\n");
   return 0;
}
