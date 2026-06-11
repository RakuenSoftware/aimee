#include <assert.h>
#include <stdio.h>
#include <string.h>

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
   test_cost_for_delegation();
   db1_shutdown();
   printf("test_token_audit: ok\n");
   return 0;
}
