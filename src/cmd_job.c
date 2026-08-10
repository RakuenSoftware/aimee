/* cmd_job.c: coordinated parallel job management CLI */
#include "aimee.h"
#include "commands.h"
#include "agent_coord.h"
#include "agent_tasks.h"
#include "cmd_agent_delegate_impl.h"
#include <aimee/delegates/delegate_economics.h>
#include <aimee/delegates/delegate_patch_coordinator.h>
#include "db1.h"
#include "cJSON.h"

typedef struct
{
   int done;
   int partial;
   int blocked;
   int failed;
   int needs_supervisor;
   int invalid;
} handoff_counts_t;

static const char *coord_task_handoff_status(const db1_coord_task_t *task,
                                             delegate_handoff_validation_t *validation)
{
   if (validation)
      memset(validation, 0, sizeof(*validation));
   if (!task)
      return "unknown";
   if (strcmp(task->status, "failed") == 0)
      return "failed";
   if (strcmp(task->status, "done") != 0)
      return task->status;

   if (!task->result[0])
   {
      if (validation)
      {
         snprintf(validation->status, sizeof(validation->status), "%s", "needs_supervisor_review");
         snprintf(validation->error, sizeof(validation->error), "%s",
                  "completed task has no structured handoff result");
      }
      return "needs_supervisor_review";
   }

   if (!validation)
      return "done";
   if (delegate_handoff_validate_text(task->result, task->files, 1, validation) != 0)
      return "needs_supervisor_review";
   return validation->status[0] ? validation->status : "needs_supervisor_review";
}

static void handoff_counts_add(handoff_counts_t *counts, const char *status,
                               const delegate_handoff_validation_t *validation)
{
   if (!counts || !status)
      return;
   if (validation && !validation->valid && strcmp(status, "needs_supervisor_review") == 0)
      counts->invalid++;
   if (strcmp(status, "done") == 0)
      counts->done++;
   else if (strcmp(status, "partial") == 0)
      counts->partial++;
   else if (strcmp(status, "blocked") == 0)
      counts->blocked++;
   else if (strcmp(status, "failed") == 0)
      counts->failed++;
   else if (strcmp(status, "needs_supervisor_review") == 0)
      counts->needs_supervisor++;
}

/* --- job start <plan_id> [--parallel N] --- */

static void job_start(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee job start <plan_id> [--parallel N]\n");
      return;
   }

   int plan_id = atoi(argv[0]);
   if (plan_id <= 0)
   {
      fprintf(stderr, "Invalid plan_id: %s\n", argv[0]);
      return;
   }

   int max_concurrent = DB1_COORD_DEFAULT_PAR;
   for (int i = 1; i < argc - 1; i++)
   {
      if (strcmp(argv[i], "--parallel") == 0 && i + 1 < argc)
         max_concurrent = atoi(argv[++i]);
   }

   /* Load plan to get steps */
   plan_t plan;
   if (db1_execution_plan_get(plan_id, &plan) != 0)
   {
      fprintf(stderr, "Plan %d not found\n", plan_id);
      return;
   }

   int job_id = db1_coord_job_create(plan_id, max_concurrent);
   if (job_id < 0)
   {
      fprintf(stderr, "Failed to create job\n");
      return;
   }

   /* Create tasks from plan steps */
   int added = 0;
   for (int i = 0; i < plan.step_count; i++)
   {
      /* Extract file paths from the step action if present.
         For now, use an empty file list; the caller can populate via MCP. */
      int tid = db1_coord_job_add_task(job_id, plan.steps[i].id, "[]", "", "", "", "");
      if (tid > 0)
         added++;
   }

   if (ctx->json_output)
   {
      printf("{\"job_id\":%d,\"plan_id\":%d,\"tasks\":%d,\"max_concurrent\":%d}\n", job_id, plan_id,
             added, max_concurrent);
   }
   else
   {
      printf("Created job #%d from plan #%d: %d tasks, max %d concurrent\n", job_id, plan_id, added,
             max_concurrent);
   }
}

/* --- job status <job_id> --- */

static void job_status_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee job status <job_id>\n");
      return;
   }

   int job_id = atoi(argv[0]);
   if (job_id <= 0)
   {
      fprintf(stderr, "Invalid job_id: %s\n", argv[0]);
      return;
   }

   db1_coord_job_t job;
   if (db1_coord_job_get(job_id, &job) != 0)
   {
      fprintf(stderr, "Job %d not found\n", job_id);
      return;
   }

   db1_coord_task_t tasks[DB1_COORD_MAX_TASKS];
   int count = db1_coord_job_list_tasks(job_id, tasks, DB1_COORD_MAX_TASKS);
   handoff_counts_t handoffs = {0};
   for (int i = 0; i < count; i++)
   {
      delegate_handoff_validation_t v = {0};
      const char *hs = coord_task_handoff_status(&tasks[i], &v);
      handoff_counts_add(&handoffs, hs, &v);
   }

   agent_config_t agent_cfg;
   agent_config_t *agent_cfg_ptr = NULL;
   memset(&agent_cfg, 0, sizeof(agent_cfg));
   if (agent_load_config(&agent_cfg) == 0)
      agent_cfg_ptr = &agent_cfg;

   delegate_economics_report_t econ;
   delegate_economics_build_report(&job, tasks, count, agent_cfg_ptr, &econ);
   delegate_patch_report_t patches;
   delegate_patch_coordinator_build_report(&job, tasks, count, &patches);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "id", job.id);
      cJSON_AddNumberToObject(obj, "plan_id", job.plan_id);
      cJSON_AddStringToObject(obj, "status", job.status);
      cJSON_AddNumberToObject(obj, "max_concurrent", job.max_concurrent);
      cJSON_AddNumberToObject(obj, "total", job.total_tasks);
      cJSON_AddNumberToObject(obj, "done", job.done_tasks);
      cJSON_AddNumberToObject(obj, "failed", job.failed_tasks);
      cJSON_AddNumberToObject(obj, "running", job.running_tasks);
      cJSON_AddNumberToObject(obj, "handoff_done", handoffs.done);
      cJSON_AddNumberToObject(obj, "handoff_partial", handoffs.partial);
      cJSON_AddNumberToObject(obj, "handoff_blocked", handoffs.blocked);
      cJSON_AddNumberToObject(obj, "handoff_failed", handoffs.failed);
      cJSON_AddNumberToObject(obj, "handoff_needs_supervisor_review", handoffs.needs_supervisor);
      cJSON_AddNumberToObject(obj, "handoff_invalid", handoffs.invalid);
      delegate_economics_add_json(obj, &econ);
      delegate_patch_coordinator_add_json(obj, &patches);
      char *json = cJSON_PrintUnformatted(obj);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(obj);
   }
   else
   {
      printf("Job #%d (plan #%d): %s\n", job.id, job.plan_id, job.status);
      printf("  Max concurrent: %d\n", job.max_concurrent);
      printf("  Total:   %d\n", job.total_tasks);
      printf("  Done:    %d\n", job.done_tasks);
      printf("  Running: %d\n", job.running_tasks);
      printf("  Failed:  %d\n", job.failed_tasks);
      printf("  Pending: %d\n",
             job.total_tasks - job.done_tasks - job.failed_tasks - job.running_tasks);
      printf("  Handoffs: done %d, partial %d, blocked %d, failed %d, needs supervisor %d",
             handoffs.done, handoffs.partial, handoffs.blocked, handoffs.failed,
             handoffs.needs_supervisor);
      if (handoffs.invalid > 0)
         printf(" (invalid %d)", handoffs.invalid);
      printf("\n");

      printf("\nDelegation report\n");
      printf("  Cost model: %s\n", econ.cost_model_label);
      printf("  Delegates: %d total, %d tier-0, %d tier-1, %d tier-2, %d tier-3",
             econ.delegate_count, econ.tier_counts[0], econ.tier_counts[1], econ.tier_counts[2],
             econ.tier_counts[3]);
      if (econ.unknown_tier_count > 0)
         printf(", %d unknown-tier", econ.unknown_tier_count);
      printf("\n");
      printf("  Delegate tokens estimated: %d", econ.delegate_tokens_estimated);
      if (econ.tokenized_delegate_results == 0)
         printf(" (unavailable)");
      printf("\n");
      printf("  Supervisor prompt tokens estimated: %d\n", econ.supervisor_prompt_tokens_estimated);
      printf("  Focused tests run by delegates: %d/%d\n", econ.delegates_with_focused_tests,
             econ.delegate_count);
      printf("  Structured handoffs valid: %d/%d", econ.valid_handoffs, econ.handoff_count);
      if (econ.invalid_handoffs > 0)
         printf(" (invalid %d)", econ.invalid_handoffs);
      printf("\n");
      printf("  Manual integration events: %d\n", econ.manual_integration_events);
      printf("  Reviewer delegates found: %d blocking\n", econ.reviewer_findings_blocking);
      printf("  Supervisor work remaining: %d decision%s\n", econ.supervisor_actions_required,
             econ.supervisor_actions_required == 1 ? "" : "s");
      printf("  Verdict: %s\n", econ.verdict_label);
      printf("  Recommendation: %s\n", econ.recommendation);

      char patch_brief[1024];
      printf("\nPatch coordinator\n");
      printf("  %s\n",
             delegate_patch_coordinator_brief(&patches, patch_brief, sizeof(patch_brief)));
   }

   /* Show individual tasks */
   if (count > 0 && !ctx->json_output)
   {
      printf("\nTasks:\n");
      for (int i = 0; i < count; i++)
      {
         delegate_handoff_validation_t v = {0};
         const char *hs = coord_task_handoff_status(&tasks[i], &v);
         printf("  #%-4d %-10s", tasks[i].id, tasks[i].status);
         if (tasks[i].claimed_by[0])
            printf(" [%s]", tasks[i].claimed_by);
         if (tasks[i].step_id > 0)
            printf(" step:%d", tasks[i].step_id);
         if (tasks[i].files[0] && strcmp(tasks[i].files, "[]") != 0)
            printf(" files:%s", tasks[i].files);
         if (strcmp(tasks[i].status, "done") == 0)
         {
            printf(" handoff:%s", hs);
            if (v.error[0])
               printf(" note:%s", v.error);
         }
         if (i < patches.task_count && patches.tasks[i].patch_state[0])
         {
            printf(" patch:%s", patches.tasks[i].patch_state);
            if (patches.tasks[i].note[0])
               printf(" patch_note:%s", patches.tasks[i].note);
         }
         printf("\n");
      }
   }
}

/* --- job cancel <job_id> --- */

static void job_cancel_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee job cancel <job_id>\n");
      return;
   }

   int job_id = atoi(argv[0]);
   if (job_id <= 0)
   {
      fprintf(stderr, "Invalid job_id: %s\n", argv[0]);
      return;
   }

   if (db1_coord_job_cancel(job_id) != 0)
   {
      fprintf(stderr, "Failed to cancel job %d\n", job_id);
      return;
   }

   printf("Cancelled job #%d\n", job_id);
}

/* --- job list --- */

static void job_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   db1_coord_job_t jobs[20];
   int count = db1_coord_job_list_recent(jobs, 20);
   if (count < 0)
      return;

   int found = 0;
   if (!ctx->json_output)
      printf("%-6s %-8s %-12s %-6s %s\n", "ID", "Plan", "Status", "Para", "Created");

   for (int i = 0; i < count; i++)
   {
      found++;
      if (ctx->json_output)
      {
         printf("{\"id\":%d,\"plan_id\":%d,\"status\":\"%s\","
                "\"max_concurrent\":%d,\"created_at\":\"%s\"}\n",
                jobs[i].id, jobs[i].plan_id, jobs[i].status, jobs[i].max_concurrent,
                jobs[i].created_at);
      }
      else
      {
         printf("%-6d %-8d %-12s %-6d %s\n", jobs[i].id, jobs[i].plan_id, jobs[i].status,
                jobs[i].max_concurrent, jobs[i].created_at);
      }
   }

   if (!found && !ctx->json_output)
      printf("No coordinated jobs found.\n");
}

/* --- Subcmd table --- */

static const subcmd_t job_subcmds[] = {
    {"start", "Create a coordinated job from a plan", job_start},
    {"status", "Show job progress and task details", job_status_cmd},
    {"show", "Alias for status", job_status_cmd},
    {"cancel", "Cancel a job and its pending tasks", job_cancel_cmd},
    {"list", "List recent coordinated jobs", job_list},
    {NULL, NULL, NULL},
};

const subcmd_t *get_job_subcmds(void)
{
   return job_subcmds;
}

void cmd_job(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("job", job_subcmds);
      return;
   }

   cmd_require_db1("cannot initialize DB1");

   subcmd_dispatch(job_subcmds, argv[0], ctx, argc - 1, argv + 1);
}
