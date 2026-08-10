/* delegate_patch_coordinator.c: the seam to the delegates module's integration
 * policy, plus the JSON and text rendering of the report it produces.
 *
 * Deciding where each packet stands -- planned, running, returned, reviewable,
 * failed or needs_supervisor -- weighs a believable handoff, ownership, base
 * staleness, focused verification and file overlap against packets already
 * declared reviewable. That is a judgement, so it is now
 * server-go/modules/delegates/patchcoord.go. Moving it also dropped a bus round
 * trip per task: the handoff rule lives in that module, so the coordinator
 * calls it directly.
 *
 * Fails closed as an EMPTY report: no packets, reviewer status "not_run".
 * Nothing is reported reviewable, which is the one answer that must never be
 * invented -- "safe to integrate" is exactly the claim a supervisor acts on.
 */
#include <aimee/delegates/delegate_patch_coordinator.h>
#include "cmd_agent_delegate_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src)
{
   if (dst && cap > 0)
      snprintf(dst, cap, "%s", src ? src : "");
}

static delegate_patch_provider_fn g_patch_provider;

void delegate_register_patch_provider(delegate_patch_provider_fn provider)
{
   g_patch_provider = provider;
}

void delegate_patch_coordinator_build_report(const db1_coord_job_t *job,
                                             const db1_coord_task_t *tasks, int task_count,
                                             delegate_patch_report_t *out)
{
   (void)job;
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   copy_str(out->reviewer_status, sizeof(out->reviewer_status), "not_run");
   copy_str(out->recommended_next_command, sizeof(out->recommended_next_command),
            "./aimee git verify");
   if (!g_patch_provider)
      return;
   g_patch_provider(tasks, task_count, out);
}

static void add_task_json(cJSON *arr, const delegate_patch_task_report_t *task)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "task_id", task->task_id);
   cJSON_AddNumberToObject(obj, "step_id", task->step_id);
   cJSON_AddStringToObject(obj, "task_status", task->task_status);
   cJSON_AddStringToObject(obj, "patch_state", task->patch_state);
   if (task->handoff_status[0])
      cJSON_AddStringToObject(obj, "handoff_status", task->handoff_status);
   cJSON_AddBoolToObject(obj, "handoff_valid", task->handoff_valid);
   cJSON_AddNumberToObject(obj, "changed_files", task->changed_files_count);
   cJSON_AddNumberToObject(obj, "passed_tests", task->passed_tests);
   cJSON_AddNumberToObject(obj, "outside_ownership_touches", task->outside_ownership_count);
   if (task->overlap_task_id > 0)
      cJSON_AddNumberToObject(obj, "overlap_task_id", task->overlap_task_id);
   cJSON_AddBoolToObject(obj, "stale_base", task->stale_base);
   cJSON_AddNumberToObject(obj, "supervisor_actions", task->supervisor_actions);
   if (task->note[0])
      cJSON_AddStringToObject(obj, "note", task->note);
   cJSON_AddItemToArray(arr, obj);
}

void delegate_patch_coordinator_add_json(cJSON *obj, const delegate_patch_report_t *report)
{
   if (!obj || !report)
      return;
   cJSON_AddNumberToObject(obj, "patch_implementation_packets", report->implementation_packets);
   cJSON_AddNumberToObject(obj, "patch_planned", report->planned);
   cJSON_AddNumberToObject(obj, "patch_running", report->running);
   cJSON_AddNumberToObject(obj, "patch_returned", report->returned);
   cJSON_AddNumberToObject(obj, "patch_verified", report->verified);
   cJSON_AddNumberToObject(obj, "patch_reviewable", report->reviewable);
   cJSON_AddNumberToObject(obj, "patch_accepted", report->accepted);
   cJSON_AddNumberToObject(obj, "patch_failed", report->failed);
   cJSON_AddNumberToObject(obj, "patch_needs_supervisor", report->needs_supervisor);
   cJSON_AddNumberToObject(obj, "patch_invalid_handoffs", report->invalid_handoffs);
   cJSON_AddNumberToObject(obj, "patch_outside_ownership_touches",
                           report->outside_ownership_touches);
   cJSON_AddNumberToObject(obj, "patch_overlaps", report->patch_overlaps);
   cJSON_AddNumberToObject(obj, "patch_stale_worktrees", report->stale_worktrees);
   cJSON_AddNumberToObject(obj, "patch_focused_tests_passed", report->focused_tests_passed);
   cJSON_AddStringToObject(obj, "patch_reviewer_status", report->reviewer_status);
   cJSON_AddNumberToObject(obj, "patch_reviewer_packets", report->reviewer_packets);
   cJSON_AddNumberToObject(obj, "patch_reviewer_blocking_findings",
                           report->reviewer_blocking_findings);
   cJSON_AddNumberToObject(obj, "patch_reviewer_owner_packet_routes",
                           report->reviewer_owner_packet_routes);
   cJSON_AddStringToObject(obj, "patch_recommended_next_command", report->recommended_next_command);

   cJSON *arr = cJSON_AddArrayToObject(obj, "patch_tasks");
   for (int i = 0; i < report->task_count; i++)
      add_task_json(arr, &report->tasks[i]);
}

const char *delegate_patch_coordinator_brief(const delegate_patch_report_t *report, char *buf,
                                             size_t cap)
{
   if (!buf || cap == 0)
      return "";
   if (!report)
   {
      snprintf(buf, cap, "Patch coordinator unavailable");
      return buf;
   }
   snprintf(
       buf, cap,
       "Packets: %d implementation, %d accepted, %d reviewable, %d needs supervisor, %d failed\n"
       "Verification: %d focused checks passed\n"
       "Reviewer: %s (%d blocking finding%s, %d owner-routed)\n"
       "Patch overlap: %s\n"
       "Outside ownership touches: %s\n"
       "Stale worktrees: %s\n"
       "Recommended next command: %s",
       report->implementation_packets, report->accepted, report->reviewable,
       report->needs_supervisor, report->failed, report->focused_tests_passed,
       report->reviewer_status, report->reviewer_blocking_findings,
       report->reviewer_blocking_findings == 1 ? "" : "s", report->reviewer_owner_packet_routes,
       report->patch_overlaps > 0 ? "needs supervisor" : "none",
       report->outside_ownership_touches > 0 ? "needs supervisor" : "none",
       report->stale_worktrees > 0 ? "needs supervisor" : "none", report->recommended_next_command);
   return buf;
}
