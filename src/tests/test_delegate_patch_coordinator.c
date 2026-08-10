/* test_delegate_patch_coordinator.c: read-only delegate patch-state report tests. */
#include <aimee/delegates/delegate_patch_coordinator.h>
#include "cmd_agent_delegate_impl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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
   test_brief_mentions_next_command();
   printf("delegate_patch_coordinator: all tests passed\n");
   return 0;
}
