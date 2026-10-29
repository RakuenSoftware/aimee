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
