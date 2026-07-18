/* delegate_patch_coordinator.c: read-only integration policy summary. */
#include "delegate_patch_coordinator.h"
#include "cmd_agent_delegate_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   char files[DELEGATE_PATCH_MAX_FILES][DELEGATE_PATCH_FILE_LEN];
   int count;
} patch_file_list_t;

static void copy_str(char *dst, size_t cap, const char *src)
{
   if (dst && cap > 0)
      snprintf(dst, cap, "%s", src ? src : "");
}

static int json_is_schema(cJSON *obj, const char *schema_name)
{
   cJSON *schema =
       cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, "schema_version") : NULL;
   return cJSON_IsString(schema) && strcmp(schema->valuestring, schema_name) == 0;
}

static int json_array_to_file_list(cJSON *arr, patch_file_list_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!cJSON_IsArray(arr))
      return 0;

   cJSON *item;
   cJSON_ArrayForEach(item, arr)
   {
      if (!cJSON_IsString(item) || !item->valuestring[0])
         continue;
      if (out->count >= DELEGATE_PATCH_MAX_FILES)
         break;
      copy_str(out->files[out->count++], sizeof(out->files[0]), item->valuestring);
   }
   return out->count;
}

static int file_list_contains(const patch_file_list_t *list, const char *path)
{
   if (!list || !path)
      return 0;
   for (int i = 0; i < list->count; i++)
   {
      if (strcmp(list->files[i], path) == 0)
         return 1;
   }
   return 0;
}

static void file_list_add_unique(patch_file_list_t *list, const char *path)
{
   if (!list || !path || !path[0] || list->count >= DELEGATE_PATCH_MAX_FILES)
      return;
   if (file_list_contains(list, path))
      return;
   copy_str(list->files[list->count++], sizeof(list->files[0]), path);
}

static int file_lists_overlap(const patch_file_list_t *a, const patch_file_list_t *b)
{
   if (!a || !b)
      return 0;
   for (int i = 0; i < a->count; i++)
   {
      if (file_list_contains(b, a->files[i]))
         return 1;
   }
   return 0;
}

static void find_handoff_text(cJSON *root, const char *raw, const char **handoff_text,
                              char **owned_handoff_text)
{
   *handoff_text = NULL;
   *owned_handoff_text = NULL;
   if (json_is_schema(root, "delegate_result_v1") || json_is_schema(root, "delegate_review_v1"))
   {
      *handoff_text = raw;
      return;
   }

   cJSON *response =
       cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "response") : NULL;
   if (cJSON_IsString(response) && response->valuestring[0])
   {
      *handoff_text = response->valuestring;
      return;
   }
   if (cJSON_IsObject(response))
      *owned_handoff_text = cJSON_PrintUnformatted(response);
   if (*owned_handoff_text)
      *handoff_text = *owned_handoff_text;
}

static cJSON *handoff_object(cJSON *root, const char *handoff_text, cJSON **owned_handoff)
{
   *owned_handoff = NULL;
   if (json_is_schema(root, "delegate_result_v1") || json_is_schema(root, "delegate_review_v1"))
      return root;
   if (!handoff_text || !handoff_text[0])
      return NULL;
   *owned_handoff = cJSON_Parse(handoff_text);
   if (json_is_schema(*owned_handoff, "delegate_result_v1") ||
       json_is_schema(*owned_handoff, "delegate_review_v1"))
      return *owned_handoff;
   cJSON_Delete(*owned_handoff);
   *owned_handoff = NULL;
   return NULL;
}

static int supervisor_action_count(cJSON *handoff)
{
   cJSON *actions = cJSON_IsObject(handoff)
                        ? cJSON_GetObjectItemCaseSensitive(handoff, "supervisor_actions")
                        : NULL;
   return cJSON_IsArray(actions) ? cJSON_GetArraySize(actions) : 0;
}

static int handoff_base_is_stale(cJSON *handoff)
{
   if (!cJSON_IsObject(handoff))
      return 0;
   cJSON *stale = cJSON_GetObjectItemCaseSensitive(handoff, "stale_base");
   if (!stale)
      stale = cJSON_GetObjectItemCaseSensitive(handoff, "worktree_stale");
   if (cJSON_IsTrue(stale))
      return 1;

   cJSON *base = cJSON_GetObjectItemCaseSensitive(handoff, "base_commit");
   if (!cJSON_IsString(base))
      base = cJSON_GetObjectItemCaseSensitive(handoff, "delegate_base_commit");
   if (!cJSON_IsString(base))
      base = cJSON_GetObjectItemCaseSensitive(handoff, "base_sha");

   cJSON *integration = cJSON_GetObjectItemCaseSensitive(handoff, "integration_base_commit");
   if (!cJSON_IsString(integration))
      integration = cJSON_GetObjectItemCaseSensitive(handoff, "integration_head");
   if (!cJSON_IsString(integration))
      integration = cJSON_GetObjectItemCaseSensitive(handoff, "integration_base_sha");

   return cJSON_IsString(base) && cJSON_IsString(integration) &&
          strcmp(base->valuestring, integration->valuestring) != 0;
}

static int reviewer_finding_routes(cJSON *findings)
{
   if (!cJSON_IsArray(findings))
      return 0;
   int count = 0;
   cJSON *finding;
   cJSON_ArrayForEach(finding, findings)
   {
      cJSON *owner = cJSON_IsObject(finding)
                         ? cJSON_GetObjectItemCaseSensitive(finding, "owner_packet")
                         : NULL;
      if (cJSON_IsString(owner) && owner->valuestring[0])
         count++;
   }
   return count;
}

static int reviewer_blocking_count(cJSON *findings)
{
   if (!cJSON_IsArray(findings))
      return 0;
   int count = 0;
   cJSON *finding;
   cJSON_ArrayForEach(finding, findings)
   {
      cJSON *severity =
          cJSON_IsObject(finding) ? cJSON_GetObjectItemCaseSensitive(finding, "severity") : NULL;
      if (!cJSON_IsString(severity))
      {
         count++;
         continue;
      }
      if (strcmp(severity->valuestring, "note") != 0 && strcmp(severity->valuestring, "low") != 0)
         count++;
   }
   return count;
}

static void add_state(delegate_patch_report_t *report, const char *state)
{
   if (!report || !state)
      return;
   if (strcmp(state, "planned") == 0)
      report->planned++;
   else if (strcmp(state, "running") == 0)
      report->running++;
   else if (strcmp(state, "returned") == 0)
      report->returned++;
   else if (strcmp(state, "verified") == 0)
      report->verified++;
   else if (strcmp(state, "reviewable") == 0)
      report->reviewable++;
   else if (strcmp(state, "accepted") == 0)
      report->accepted++;
   else if (strcmp(state, "failed") == 0)
      report->failed++;
   else if (strcmp(state, "needs_supervisor") == 0)
      report->needs_supervisor++;
}

static void set_task_state(delegate_patch_task_report_t *tr, delegate_patch_report_t *report,
                           const char *state, const char *note)
{
   copy_str(tr->patch_state, sizeof(tr->patch_state), state);
   if (note && note[0])
      copy_str(tr->note, sizeof(tr->note), note);
   add_state(report, state);
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

static void process_reviewer(delegate_patch_report_t *report, cJSON *handoff)
{
   report->reviewer_packets++;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(handoff, "status");
   if (cJSON_IsString(status) && status->valuestring[0])
      copy_str(report->reviewer_status, sizeof(report->reviewer_status), status->valuestring);
   else
      copy_str(report->reviewer_status, sizeof(report->reviewer_status), "needs_supervisor");

   cJSON *findings = cJSON_GetObjectItemCaseSensitive(handoff, "findings");
   if (strcmp(report->reviewer_status, "block") == 0)
      report->reviewer_blocking_findings += reviewer_blocking_count(findings);
   report->reviewer_owner_packet_routes += reviewer_finding_routes(findings);
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
   if (!tasks || task_count <= 0)
      return;

   patch_file_list_t reviewable_files[DB1_COORD_MAX_TASKS];
   int reviewable_task_ids[DB1_COORD_MAX_TASKS];
   int reviewable_count = 0;
   memset(reviewable_files, 0, sizeof(reviewable_files));
   memset(reviewable_task_ids, 0, sizeof(reviewable_task_ids));

   for (int i = 0; i < task_count && out->task_count < DB1_COORD_MAX_TASKS; i++)
   {
      const db1_coord_task_t *task = &tasks[i];
      delegate_patch_task_report_t *tr = &out->tasks[out->task_count++];
      tr->task_id = task->id;
      tr->step_id = task->step_id;
      copy_str(tr->task_status, sizeof(tr->task_status), task->status);

      if (strcmp(task->status, "pending") == 0)
      {
         out->implementation_packets++;
         set_task_state(tr, out, "planned", "packet has not launched");
         continue;
      }
      if (strcmp(task->status, "claimed") == 0 || strcmp(task->status, "running") == 0)
      {
         out->implementation_packets++;
         set_task_state(tr, out, "running", "delegate is active");
         continue;
      }
      if (strcmp(task->status, "failed") == 0)
      {
         out->implementation_packets++;
         set_task_state(tr, out, "failed", task->error[0] ? task->error : "delegate failed");
         continue;
      }

      cJSON *root = task->result[0] ? cJSON_Parse(task->result) : NULL;
      const char *handoff_text = NULL;
      char *owned_handoff_text = NULL;
      find_handoff_text(root, task->result, &handoff_text, &owned_handoff_text);

      cJSON *owned_handoff = NULL;
      cJSON *handoff = handoff_object(root, handoff_text, &owned_handoff);
      if (json_is_schema(handoff, "delegate_review_v1"))
      {
         process_reviewer(out, handoff);
         set_task_state(tr, out, "reviewer", "read-only reviewer result");
         cJSON_Delete(owned_handoff);
         free(owned_handoff_text);
         cJSON_Delete(root);
         continue;
      }

      out->implementation_packets++;
      delegate_handoff_validation_t v;
      memset(&v, 0, sizeof(v));
      if (strcmp(task->status, "done") != 0 || !handoff_text ||
          delegate_handoff_validate_text(handoff_text, task->files, 1, &v) != 0)
      {
         out->invalid_handoffs++;
         copy_str(tr->handoff_status, sizeof(tr->handoff_status), "needs_supervisor_review");
         set_task_state(tr, out, "needs_supervisor",
                        v.error[0] ? v.error : "missing or invalid structured handoff");
         cJSON_Delete(owned_handoff);
         free(owned_handoff_text);
         cJSON_Delete(root);
         continue;
      }

      tr->handoff_valid = v.valid;
      copy_str(tr->handoff_status, sizeof(tr->handoff_status), v.status);
      tr->changed_files_count = v.changed_files_count;
      tr->passed_tests = v.passed_tests;
      tr->outside_ownership_count = v.outside_ownership_count;
      tr->supervisor_actions = supervisor_action_count(handoff);
      tr->stale_base = handoff_base_is_stale(handoff);
      out->focused_tests_passed += v.passed_tests;
      out->outside_ownership_touches += v.outside_ownership_count;
      if (tr->stale_base)
         out->stale_worktrees++;
      if (v.passed_tests > 0)
         out->verified++;

      cJSON *changed = cJSON_IsObject(handoff)
                           ? cJSON_GetObjectItemCaseSensitive(handoff, "changed_files")
                           : NULL;
      patch_file_list_t changed_files;
      json_array_to_file_list(changed, &changed_files);

      if (strcmp(v.status, "failed") == 0)
         set_task_state(tr, out, "failed", v.error[0] ? v.error : "delegate reported failed");
      else if (strcmp(v.status, "blocked") == 0)
         set_task_state(tr, out, "needs_supervisor",
                        v.error[0] ? v.error : "delegate reported blocked");
      else if (v.outside_ownership_count > 0)
         set_task_state(tr, out, "needs_supervisor",
                        v.error[0] ? v.error : "changed files outside owned_files");
      else if (tr->stale_base)
         set_task_state(tr, out, "needs_supervisor", "delegate base differs from integration base");
      else if (v.passed_tests <= 0)
         set_task_state(tr, out, "returned",
                        v.error[0] ? v.error : "focused verification not reported");
      else
      {
         int overlap_task_id = 0;
         for (int r = 0; r < reviewable_count; r++)
         {
            if (file_lists_overlap(&changed_files, &reviewable_files[r]))
            {
               overlap_task_id = reviewable_task_ids[r];
               break;
            }
         }
         if (overlap_task_id > 0)
         {
            tr->overlap_task_id = overlap_task_id;
            out->patch_overlaps++;
            set_task_state(tr, out, "needs_supervisor", "changed files overlap another packet");
         }
         else
         {
            for (int f = 0; f < changed_files.count; f++)
               file_list_add_unique(&reviewable_files[reviewable_count], changed_files.files[f]);
            reviewable_task_ids[reviewable_count++] = task->id;
            set_task_state(tr, out, "reviewable", "ownership and verification checks passed");
         }
      }

      cJSON_Delete(owned_handoff);
      free(owned_handoff_text);
      cJSON_Delete(root);
   }
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
