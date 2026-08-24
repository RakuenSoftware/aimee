/* cmd_autopilot.c: `aimee autopilot` CLI command */

#include "aimee.h"
#include "commands.h"
#include "db1_client/db1.h"
#include "headers/agent_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- subcommand handlers --- */

static void autopilot_start(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee autopilot start \"task description\" [--plan-id <id>] "
                      "[--plan-depth trivial|simple|complex]\n");
      return;
   }

   /* Collect the task from positional args (allow multi-word without quotes) */
   char task[DB1_PIPE_TASK_LEN] = "";
   int plan_id = 0;
   const char *plan_depth = NULL;
   size_t used = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--plan-id") == 0 && i + 1 < argc)
      {
         plan_id = atoi(argv[++i]);
         continue;
      }
      if (strcmp(argv[i], "--plan-depth") == 0 && i + 1 < argc)
      {
         plan_depth = argv[++i];
         continue;
      }
      if (used > 0 && used < sizeof(task) - 1)
         task[used++] = ' ';
      size_t rem = sizeof(task) - used - 1;
      size_t n = strlen(argv[i]);
      if (n > rem)
         n = rem;
      memcpy(task + used, argv[i], n);
      used += n;
   }
   task[used] = '\0';

   if (!task[0])
   {
      fprintf(stderr, "error: task description required\n");
      return;
   }

   char classification[16] = "";
   pipeline_classify_task(task, plan_depth, classification, sizeof(classification));
   int pipeline_id = 0;
   if (db1_pipeline_create(task, classification, classification, &pipeline_id) != 0)
   {
      fprintf(stderr, "error: could not create pipeline\n");
      return;
   }

   if (plan_id > 0)
   {
      (void)db1_pipeline_link_plan(pipeline_id, plan_id);
   }

   char msg[PIPELINE_MAX_MSG_LEN] = "";
   pipeline_advance(pipeline_id, msg, sizeof(msg));

   char report[PIPELINE_MAX_MSG_LEN];
   pipeline_status_report(pipeline_id, report, sizeof(report));

   printf("pipeline #%d created\n\n%s\n\n%s\n", pipeline_id, report, msg);
}

static void autopilot_advance(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee autopilot advance <pipeline_id>\n");
      return;
   }
   int pipeline_id = atoi(argv[0]);
   if (pipeline_id <= 0)
   {
      fprintf(stderr, "error: invalid pipeline_id\n");
      return;
   }

   char msg[PIPELINE_MAX_MSG_LEN] = "";
   int rc = pipeline_advance(pipeline_id, msg, sizeof(msg));

   char report[PIPELINE_MAX_MSG_LEN];
   pipeline_status_report(pipeline_id, report, sizeof(report));

   const char *label = (rc == PIPELINE_ADV_DONE)     ? "complete"
                       : (rc == PIPELINE_ADV_PAUSED) ? "paused"
                       : (rc == PIPELINE_ADV_ERROR)  ? "error"
                                                     : "progressed";
   printf("pipeline #%d: %s\n\n%s\n\n%s\n", pipeline_id, label, report, msg);
}

static void autopilot_status(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee autopilot status <pipeline_id>\n");
      return;
   }
   int pipeline_id = atoi(argv[0]);
   char buf[PIPELINE_MAX_MSG_LEN];
   pipeline_status_report(pipeline_id, buf, sizeof(buf));
   printf("%s\n", buf);
}

static void autopilot_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
   char buf[8192];
   int n = pipeline_list(buf, sizeof(buf), 0);
   if (n == 0)
      printf("no pipelines found\n");
   else
      printf("%s", buf);
}

static void autopilot_cancel(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee autopilot cancel <pipeline_id>\n");
      return;
   }
   int pipeline_id = atoi(argv[0]);
   db1_pipeline_t p;
   int rc = (db1_pipeline_get(pipeline_id, &p) == 0) ? db1_pipeline_cancel(pipeline_id) : -1;
   if (rc == 0)
      printf("pipeline #%d cancelled\n", pipeline_id);
   else
      fprintf(stderr, "error: could not cancel pipeline #%d\n", pipeline_id);
}

static void autopilot_resume(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee autopilot resume <pipeline_id>\n");
      return;
   }
   int pipeline_id = atoi(argv[0]);
   db1_pipeline_t p;
   int rc = -1;
   if (db1_pipeline_get(pipeline_id, &p) == 0 && strcmp(p.status, PIPE_STATUS_PAUSED) == 0)
      rc = db1_pipeline_update(pipeline_id, PIPE_STATUS_ACTIVE, p.current_phase, 0, p.plan_id,
                               p.job_id, p.request_classification, p.plan_depth,
                               p.clarify_session_id);
   if (rc == 0)
      printf("pipeline #%d resumed — run 'aimee autopilot advance %d' to continue\n", pipeline_id,
             pipeline_id);
   else
      fprintf(stderr, "error: could not resume pipeline #%d (not paused?)\n", pipeline_id);
}

static void autopilot_link_plan(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 2)
   {
      fprintf(stderr, "usage: aimee autopilot link-plan <pipeline_id> <plan_id>\n");
      return;
   }
   int pipeline_id = atoi(argv[0]);
   int plan_id = atoi(argv[1]);
   int rc = db1_pipeline_link_plan(pipeline_id, plan_id);
   if (rc == 0)
      printf("plan #%d linked to pipeline #%d\n", plan_id, pipeline_id);
   else
      fprintf(stderr, "error: could not link plan\n");
}

static void autopilot_link_job(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 2)
   {
      fprintf(stderr, "usage: aimee autopilot link-job <pipeline_id> <job_id>\n");
      return;
   }
   int pipeline_id = atoi(argv[0]);
   int job_id = atoi(argv[1]);
   int rc = db1_pipeline_link_job(pipeline_id, job_id);
   if (rc == 0)
      printf("job #%d linked to pipeline #%d\n", job_id, pipeline_id);
   else
      fprintf(stderr, "error: could not link job\n");
}

/* --- subcmd table --- */

static const subcmd_t autopilot_subcmds[] = {
    {"start", "Start an autonomous pipeline for a task", autopilot_start},
    {"advance", "Advance a pipeline one step", autopilot_advance},
    {"status", "Show pipeline status", autopilot_status},
    {"list", "List pipelines", autopilot_list},
    {"cancel", "Cancel a pipeline", autopilot_cancel},
    {"resume", "Resume a paused pipeline", autopilot_resume},
    {"link-plan", "Link an execution plan to a pipeline", autopilot_link_plan},
    {"link-job", "Link a coord job to a pipeline", autopilot_link_job},
    {NULL, NULL, NULL},
};

const subcmd_t *get_autopilot_subcmds(void)
{
   return autopilot_subcmds;
}

/* --- main entry point --- */

void cmd_autopilot(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("autopilot", autopilot_subcmds);
      return;
   }

   /* Special case: `aimee autopilot "task"` without a subcommand keyword
    * starts a new pipeline if the first arg doesn't match a subcommand. */
   int is_subcmd = 0;
   for (int i = 0; autopilot_subcmds[i].name; i++)
   {
      if (strcmp(argv[0], autopilot_subcmds[i].name) == 0)
      {
         is_subcmd = 1;
         break;
      }
   }

   cmd_require_db1("autopilot: could not initialize DB1");

   if (!is_subcmd)
   {
      /* Treat the entire argv as a task description */
      autopilot_start(ctx, argc, argv);
   }
   else
   {
      subcmd_dispatch(autopilot_subcmds, argv[0], ctx, argc - 1, argv + 1);
   }
}
