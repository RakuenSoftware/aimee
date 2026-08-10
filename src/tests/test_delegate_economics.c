/* test_delegate_economics.c: supervisor-centric delegate economics tests */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <aimee/delegates/delegate_economics.h>
#include "cmd_agent_delegate_impl.h"
#include "cJSON.h"

static void add_agent(agent_config_t *cfg, int idx, const char *name, int tier)
{
   assert(idx >= 0 && idx < MAX_AGENTS);
   snprintf(cfg->agents[idx].name, sizeof(cfg->agents[idx].name), "%s", name);
   cfg->agents[idx].cost_tier = tier;
   if (idx >= cfg->agent_count)
      cfg->agent_count = idx + 1;
}

static void fill_task(db1_coord_task_t *task, const char *status, const char *claimed_by,
                      const char *files, const char *result)
{
   memset(task, 0, sizeof(*task));
   snprintf(task->status, sizeof(task->status), "%s", status);
   snprintf(task->claimed_by, sizeof(task->claimed_by), "%s", claimed_by ? claimed_by : "");
   snprintf(task->files, sizeof(task->files), "%s", files ? files : "[]");
   snprintf(task->result, sizeof(task->result), "%s", result ? result : "");
}

static void handoff(char *buf, size_t len, const char *file, const char *tests, const char *actions)
{
   snprintf(buf, len,
            "{"
            "\"schema_version\":\"delegate_result_v1\","
            "\"status\":\"done\","
            "\"changed_files\":[\"%s\"],"
            "\"tests\":%s,"
            "\"supervisor_actions\":%s,"
            "\"summary\":\"done\""
            "}",
            file, tests, actions);
}

static void test_tier_distribution_and_handoff_counts(void)
{
   agent_config_t cfg = {0};
   add_agent(&cfg, 0, "free-a", 0);
   add_agent(&cfg, 1, "cheap-b", 1);
   add_agent(&cfg, 2, "expensive-c", 3);

   char h1[512];
   char h2[512];
   handoff(h1, sizeof(h1), "src/free.c", "[{\"name\":\"unit-free\",\"status\":\"passed\"}]", "[]");
   handoff(h2, sizeof(h2), "src/cheap.c", "[{\"name\":\"unit-cheap\",\"status\":\"passed\"}]",
           "[\"choose public name\"]");

   db1_coord_task_t tasks[4];
   fill_task(&tasks[0], "done", "free-a", "[\"src/free.c\"]", h1);
   fill_task(&tasks[1], "done", "cheap-b", "[\"src/cheap.c\"]", h2);
   fill_task(&tasks[2], "failed", "expensive-c", "[\"src/fail.c\"]", "compile failed");
   fill_task(&tasks[3], "done", "missing-agent", "[\"src/bad.c\"]", "not json");

   delegate_economics_report_t report;
   delegate_economics_build_report(NULL, tasks, 4, &cfg, &report);

   assert(report.delegate_count == 4);
   assert(report.tier_counts[0] == 1);
   assert(report.tier_counts[1] == 1);
   assert(report.tier_counts[3] == 1);
   assert(report.unknown_tier_count == 1);
   assert(report.handoff_count == 3);
   assert(report.valid_handoffs == 2);
   assert(report.invalid_handoffs == 1);
   assert(report.focused_tests_run_by_delegates == 2);
   assert(report.delegates_with_focused_tests == 2);
   assert(report.supervisor_actions_required == 2);
   assert(report.manual_integration_events == 3);
   printf("  PASS: test_tier_distribution_and_handoff_counts\n");
}

static void test_tier0_heavy_verdict_recommends_broader_delegation(void)
{
   agent_config_t cfg = {0};
   add_agent(&cfg, 0, "free-a", 0);
   add_agent(&cfg, 1, "free-b", 0);
   add_agent(&cfg, 2, "cheap-c", 1);

   char h1[512];
   char h2[512];
   char h3[512];
   handoff(h1, sizeof(h1), "src/a.c", "[{\"name\":\"unit-a\",\"status\":\"passed\"}]", "[]");
   handoff(h2, sizeof(h2), "src/b.c", "[{\"name\":\"unit-b\",\"status\":\"passed\"}]", "[]");
   handoff(h3, sizeof(h3), "src/c.c", "[{\"name\":\"unit-c\",\"status\":\"passed\"}]", "[]");

   db1_coord_task_t tasks[3];
   fill_task(&tasks[0], "done", "free-a", "[\"src/a.c\"]", h1);
   fill_task(&tasks[1], "done", "free-b", "[\"src/b.c\"]", h2);
   fill_task(&tasks[2], "done", "cheap-c", "[\"src/c.c\"]", h3);

   delegate_economics_report_t report;
   delegate_economics_build_report(NULL, tasks, 3, &cfg, &report);

   assert(delegate_economics_is_tier0_heavy(&report));
   assert(strcmp(report.verdict, "likely_net_win") == 0);
   assert(strstr(report.recommendation, "broader delegation") != NULL);
   printf("  PASS: test_tier0_heavy_verdict_recommends_broader_delegation\n");
}

static void test_mixed_and_expensive_only_verdicts(void)
{
   agent_config_t mixed = {0};
   add_agent(&mixed, 0, "free-a", 0);
   add_agent(&mixed, 1, "expensive-a", 3);
   add_agent(&mixed, 2, "expensive-b", 3);

   char h1[512];
   char h2[512];
   char h3[512];
   handoff(h1, sizeof(h1), "src/a.c", "[{\"name\":\"unit-a\",\"status\":\"passed\"}]", "[]");
   handoff(h2, sizeof(h2), "src/b.c", "[{\"name\":\"unit-b\",\"status\":\"passed\"}]", "[]");
   handoff(h3, sizeof(h3), "src/c.c", "[{\"name\":\"unit-c\",\"status\":\"passed\"}]", "[]");

   db1_coord_task_t mixed_tasks[3];
   fill_task(&mixed_tasks[0], "done", "free-a", "[\"src/a.c\"]", h1);
   fill_task(&mixed_tasks[1], "done", "expensive-a", "[\"src/b.c\"]", h2);
   fill_task(&mixed_tasks[2], "done", "expensive-b", "[\"src/c.c\"]", h3);

   delegate_economics_report_t report;
   delegate_economics_build_report(NULL, mixed_tasks, 3, &mixed, &report);
   assert(strcmp(report.verdict, "unclear") == 0);

   agent_config_t expensive = {0};
   add_agent(&expensive, 0, "expensive-a", 3);
   add_agent(&expensive, 1, "expensive-b", 3);

   char no_test_a[512];
   char no_test_b[512];
   handoff(no_test_a, sizeof(no_test_a), "src/a.c", "[]", "[]");
   handoff(no_test_b, sizeof(no_test_b), "src/b.c", "[]", "[]");

   db1_coord_task_t expensive_tasks[2];
   fill_task(&expensive_tasks[0], "done", "expensive-a", "[\"src/a.c\"]", no_test_a);
   fill_task(&expensive_tasks[1], "done", "expensive-b", "[\"src/b.c\"]", no_test_b);

   delegate_economics_build_report(NULL, expensive_tasks, 2, &expensive, &report);
   assert(strcmp(report.verdict, "likely_net_loss") == 0);
   printf("  PASS: test_mixed_and_expensive_only_verdicts\n");
}

static void test_tier0_heavy_high_manual_intervention_is_unclear(void)
{
   agent_config_t cfg = {0};
   add_agent(&cfg, 0, "free-a", 0);
   add_agent(&cfg, 1, "free-b", 0);

   char h1[512];
   char h2[512];
   handoff(h1, sizeof(h1), "src/a.c", "[{\"name\":\"unit-a\",\"status\":\"passed\"}]",
           "[\"resolve API naming\"]");
   handoff(h2, sizeof(h2), "src/b.c", "[{\"name\":\"unit-b\",\"status\":\"passed\"}]",
           "[\"integrate overlapping changes\"]");

   db1_coord_task_t tasks[2];
   fill_task(&tasks[0], "done", "free-a", "[\"src/a.c\"]", h1);
   fill_task(&tasks[1], "done", "free-b", "[\"src/b.c\"]", h2);

   delegate_economics_report_t report;
   delegate_economics_build_report(NULL, tasks, 2, &cfg, &report);
   assert(delegate_economics_is_tier0_heavy(&report));
   assert(report.manual_integration_events == 2);
   assert(strcmp(report.verdict, "unclear") == 0);
   assert(strstr(report.recommendation, "broader delegation") != NULL);
   printf("  PASS: test_tier0_heavy_high_manual_intervention_is_unclear\n");
}

static void test_zero_delegate_job_is_unclear(void)
{
   delegate_economics_report_t report;
   delegate_economics_build_report(NULL, NULL, 0, NULL, &report);
   assert(report.delegate_count == 0);
   assert(strcmp(report.verdict, "unclear") == 0);
   assert(report.supervisor_prompt_tokens_estimated == 0);
   printf("  PASS: test_zero_delegate_job_is_unclear\n");
}

static void test_agent_result_json_metadata(void)
{
   agent_config_t cfg = {0};
   add_agent(&cfg, 0, "free-a", 0);

   agent_result_t result = {0};
   snprintf(result.agent_name, sizeof(result.agent_name), "%s", "free-a");
   result.prompt_tokens = 11;
   result.completion_tokens = 7;
   result.cache_read_tokens = 3;
   result.cache_write_tokens = 5;

   cJSON *obj = cJSON_CreateObject();
   delegate_economics_add_agent_result_json(obj, &cfg, "review", &result, NULL);
   assert(strcmp(cJSON_GetObjectItem(obj, "agent")->valuestring, "free-a") == 0);
   assert(cJSON_GetObjectItem(obj, "agent_cost_tier")->valueint == 0);
   assert(strcmp(cJSON_GetObjectItem(obj, "delegate_cost_model")->valuestring,
                 DELEGATE_ECONOMICS_COST_MODEL) == 0);
   assert(cJSON_GetObjectItem(obj, "delegate_tokens_estimated")->valueint == 18);
   assert(cJSON_GetObjectItem(obj, "delegate_cache_read_tokens")->valueint == 3);
   cJSON_Delete(obj);
   printf("  PASS: test_agent_result_json_metadata\n");
}

/* Judging a handoff is the delegates module's rule now
 * (server-go/modules/delegates/handoff.go) and this binary hosts no bus. The
 * subject of these tests is COORDINATION -- which packets are reviewable, which
 * conflict, which need a supervisor -- so the test supplies the verdicts it
 * wants to coordinate over.
 *
 * This is deliberately NOT the rule. It does not check schema_version, status
 * admission, summary presence or the done-without-verification downgrade; it
 * reads only the two numbers these fixtures vary, so it cannot drift into a
 * second copy of a rule that lives in exactly one place. */
static int econ_test_handoff_provider(const char *text, const char *owned_files_json,
                                      int require_verification, delegate_handoff_validation_t *out)
{
   (void)require_verification;
   memset(out, 0, sizeof(*out));
   snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");
   if (!text || !text[0])
      return -1;

   cJSON *root = cJSON_Parse(text);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      snprintf(out->error, sizeof(out->error), "%s", "handoff is not valid JSON object");
      out->needs_supervisor_review = 1;
      return -1;
   }

   cJSON *changed = cJSON_GetObjectItemCaseSensitive(root, "changed_files");
   cJSON *tests = cJSON_GetObjectItemCaseSensitive(root, "tests");
   cJSON *owned = owned_files_json ? cJSON_Parse(owned_files_json) : NULL;

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, tests)
   {
      cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "status");
      if (cJSON_IsString(st) && strcmp(st->valuestring, "passed") == 0)
         out->passed_tests++;
   }
   cJSON_ArrayForEach(item, changed)
   {
      if (!cJSON_IsString(item))
         continue;
      out->changed_files_count++;
      int owned_here = 0;
      cJSON *o = NULL;
      cJSON_ArrayForEach(o, owned)
      {
         if (cJSON_IsString(o) && strcmp(o->valuestring, item->valuestring) == 0)
         {
            owned_here = 1;
            break;
         }
      }
      if (cJSON_IsArray(owned) && cJSON_GetArraySize(owned) > 0 && !owned_here)
         out->outside_ownership_count++;
   }
   cJSON_Delete(owned);

   cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "status");
   if (cJSON_IsString(raw))
   {
      snprintf(out->raw_status, sizeof(out->raw_status), "%s", raw->valuestring);
      snprintf(out->status, sizeof(out->status), "%s", raw->valuestring);
   }
   cJSON_Delete(root);

   out->valid = 1;
   if (out->outside_ownership_count > 0)
   {
      snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");
      snprintf(out->error, sizeof(out->error), "%s", "handoff touched files outside owned_files");
      out->needs_supervisor_review = 1;
   }
   return 0;
}

int main(void)
{
   delegate_register_handoff_provider(econ_test_handoff_provider);
   test_tier_distribution_and_handoff_counts();
   test_tier0_heavy_verdict_recommends_broader_delegation();
   test_mixed_and_expensive_only_verdicts();
   test_tier0_heavy_high_manual_intervention_is_unclear();
   test_zero_delegate_job_is_unclear();
   test_agent_result_json_metadata();
   printf("delegate_economics: all tests passed\n");
   return 0;
}
