/* test_delegate_patch_coordinator.c: read-only delegate patch-state report tests. */
#include <aimee/delegates/delegate_patch_coordinator.h>
#include "cmd_agent_delegate_impl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void init_task(db1_coord_task_t *task, int id, const char *status, const char *files,
                      const char *result)
{
   memset(task, 0, sizeof(*task));
   task->id = id;
   task->job_id = 1;
   task->step_id = id + 100;
   snprintf(task->status, sizeof(task->status), "%s", status);
   snprintf(task->files, sizeof(task->files), "%s", files ? files : "[]");
   if (result)
      snprintf(task->result, sizeof(task->result), "%s", result);
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
static int coord_test_handoff_provider(const char *text, const char *owned_files_json,
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

static void make_handoff(char *buf, size_t len, const char *status, const char *changed,
                         const char *tests, const char *extra)
{
   snprintf(buf, len,
            "{"
            "\"schema_version\":\"delegate_result_v1\","
            "\"status\":\"%s\","
            "\"packet_id\":\"packet-one\","
            "\"changed_files\":[%s],"
            "\"outside_ownership_touches\":[],"
            "\"tests\":[%s],"
            "\"supervisor_actions\":[],"
            "\"summary\":\"result\"%s"
            "}",
            status, changed, tests, extra ? extra : "");
}

static void test_disjoint_verified_packets_are_reviewable(void)
{
   char h1[1024], h2[1024];
   make_handoff(h1, sizeof(h1), "done", "\"src/a.c\"",
                "{\"name\":\"unit-a\",\"status\":\"passed\"}", "");
   make_handoff(h2, sizeof(h2), "done", "\"src/b.c\"",
                "{\"name\":\"unit-b\",\"status\":\"passed\"}", "");

   db1_coord_task_t tasks[2];
   init_task(&tasks[0], 1, "done", "[\"src/a.c\"]", h1);
   init_task(&tasks[1], 2, "done", "[\"src/b.c\"]", h2);

   delegate_patch_report_t report;
   delegate_patch_coordinator_build_report(NULL, tasks, 2, &report);
   assert(report.implementation_packets == 2);
   assert(report.reviewable == 2);
   assert(report.needs_supervisor == 0);
   assert(report.verified == 2);
   assert(report.focused_tests_passed == 2);
   assert(strcmp(report.tasks[0].patch_state, "reviewable") == 0);
   assert(strcmp(report.tasks[1].patch_state, "reviewable") == 0);
   printf("  PASS: test_disjoint_verified_packets_are_reviewable\n");
}

static void test_outside_owned_files_need_supervisor(void)
{
   char h[1024];
   make_handoff(h, sizeof(h), "done", "\"src/a.c\",\"src/outside.c\"",
                "{\"name\":\"unit-a\",\"status\":\"passed\"}", "");

   db1_coord_task_t task;
   init_task(&task, 3, "done", "[\"src/a.c\"]", h);

   delegate_patch_report_t report;
   delegate_patch_coordinator_build_report(NULL, &task, 1, &report);
   assert(report.needs_supervisor == 1);
   assert(report.outside_ownership_touches == 1);
   assert(strcmp(report.tasks[0].patch_state, "needs_supervisor") == 0);
   assert(strstr(report.tasks[0].note, "outside") != NULL);
   printf("  PASS: test_outside_owned_files_need_supervisor\n");
}

static void test_overlapping_patches_need_supervisor(void)
{
   char h1[1024], h2[1024];
   make_handoff(h1, sizeof(h1), "done", "\"src/shared.c\"",
                "{\"name\":\"unit-a\",\"status\":\"passed\"}", "");
   make_handoff(h2, sizeof(h2), "done", "\"src/shared.c\"",
                "{\"name\":\"unit-b\",\"status\":\"passed\"}", "");

   db1_coord_task_t tasks[2];
   init_task(&tasks[0], 4, "done", "[\"src/shared.c\"]", h1);
   init_task(&tasks[1], 5, "done", "[\"src/shared.c\"]", h2);

   delegate_patch_report_t report;
   delegate_patch_coordinator_build_report(NULL, tasks, 2, &report);
   assert(report.reviewable == 1);
   assert(report.needs_supervisor == 1);
   assert(report.patch_overlaps == 1);
   assert(report.tasks[1].overlap_task_id == 4);
   assert(strcmp(report.tasks[1].patch_state, "needs_supervisor") == 0);
   printf("  PASS: test_overlapping_patches_need_supervisor\n");
}

static void test_stale_base_metadata_needs_supervisor(void)
{
   char h[1024];
   make_handoff(h, sizeof(h), "done", "\"src/a.c\"", "{\"name\":\"unit-a\",\"status\":\"passed\"}",
                ",\"base_commit\":\"aaa\",\"integration_base_commit\":\"bbb\"");

   db1_coord_task_t task;
   init_task(&task, 6, "done", "[\"src/a.c\"]", h);

   delegate_patch_report_t report;
   delegate_patch_coordinator_build_report(NULL, &task, 1, &report);
   assert(report.stale_worktrees == 1);
   assert(report.needs_supervisor == 1);
   assert(report.tasks[0].stale_base == 1);
   assert(strcmp(report.tasks[0].patch_state, "needs_supervisor") == 0);
   printf("  PASS: test_stale_base_metadata_needs_supervisor\n");
}

static void test_reviewer_routes_owner_packet_findings(void)
{
   const char *review = "{"
                        "\"schema_version\":\"delegate_review_v1\","
                        "\"status\":\"block\","
                        "\"findings\":[{\"severity\":\"high\","
                        "\"owner_packet\":\"packet-a\","
                        "\"description\":\"missing check\"}],"
                        "\"missing_requirements\":[]"
                        "}";
   db1_coord_task_t task;
   init_task(&task, 7, "done", "[]", review);

   delegate_patch_report_t report;
   delegate_patch_coordinator_build_report(NULL, &task, 1, &report);
   assert(report.reviewer_packets == 1);
   assert(strcmp(report.reviewer_status, "block") == 0);
   assert(report.reviewer_blocking_findings == 1);
   assert(report.reviewer_owner_packet_routes == 1);
   assert(strcmp(report.tasks[0].patch_state, "reviewer") == 0);
   printf("  PASS: test_reviewer_routes_owner_packet_findings\n");
}

static void test_brief_mentions_next_command(void)
{
   delegate_patch_report_t report;
   memset(&report, 0, sizeof(report));
   snprintf(report.reviewer_status, sizeof(report.reviewer_status), "%s", "not_run");
   snprintf(report.recommended_next_command, sizeof(report.recommended_next_command), "%s",
            "./aimee git verify");
   char buf[1024];
   const char *brief = delegate_patch_coordinator_brief(&report, buf, sizeof(buf));
   assert(strstr(brief, "Recommended next command: ./aimee git verify") != NULL);
   printf("  PASS: test_brief_mentions_next_command\n");
}

int main(void)
{
   delegate_register_handoff_provider(coord_test_handoff_provider);
   printf("delegate_patch_coordinator:\n");
   test_disjoint_verified_packets_are_reviewable();
   test_outside_owned_files_need_supervisor();
   test_overlapping_patches_need_supervisor();
   test_stale_base_metadata_needs_supervisor();
   test_reviewer_routes_owner_packet_findings();
   test_brief_mentions_next_command();
   printf("delegate_patch_coordinator: all tests passed\n");
   return 0;
}
