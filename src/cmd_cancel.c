/* cmd_cancel.c: unified workflow cancellation (aimee cancel) */
#include "aimee.h"
#include "db1_client/db1.h"
#include "commands.h"

/* ---- Active workflow detection ---- */

typedef struct
{
   int active_plans;
   int plan_ids[32];
   int active_jobs;
   int job_ids[32];
   int active_delegations;
   int delegation_ids[32];
} active_workflows_t;

static int detect_active_workflows(active_workflows_t *out)
{
   memset(out, 0, sizeof(*out));

   /* Running execution plans */
   {
      int np = db1_execution_plan_list_running_ids(out->plan_ids, 32);
      if (np > 0)
         out->active_plans = np;
   }

   /* Running agent jobs */
   {
      int nj = db1_agent_job_list_running_ids(out->job_ids, 32);
      if (nj > 0)
         out->active_jobs = nj;
   }

   /* Running delegation spawns */
   int n = db1_delegation_spawn_list_active(out->delegation_ids, 32);
   if (n > 0)
      out->active_delegations = n;

   return out->active_plans + out->active_jobs + out->active_delegations;
}

/* ---- Orphan detection and cleanup ---- */

static int cleanup_orphans(void)
{
   int cleaned = 0;

   /* Orphan plans: running for > 24 hours */
   cleaned += db1_execution_plan_cancel_stale(24 * 3600, "orphan cleanup");

   /* Orphan plan steps: running but plan is not running */
   cleaned += db1_plan_step_cancel_orphans();

   /* Orphan jobs: running for > 24 hours */
   cleaned += db1_agent_job_cancel_stale(24 * 3600, "orphan cleanup");

   /* Orphan delegations: running for > 24 hours */
   cleaned += db1_delegation_spawn_cancel_stale();

   return cleaned;
}

/* ---- Subcommands ---- */

static void cancel_all(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {"force", "orphans", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   if (opt_has(&opts, "orphans"))
   {
      int cleaned = cleanup_orphans();
      printf("Orphan cleanup: %d stale item%s cancelled.\n", cleaned, cleaned == 1 ? "" : "s");
      return;
   }

   const char *reason = opt_get(&opts, "reason");
   if (!reason)
      reason = "user cancel";

   active_workflows_t wf;
   int total = detect_active_workflows(&wf);

   if (total == 0)
   {
      printf("No active workflows to cancel.\n");
      return;
   }

   int cancelled = 0;

   /* Cancel in dependency order: plans first, then jobs, then delegations */
   for (int i = 0; i < wf.active_plans; i++)
   {
      (void)db1_plan_step_cancel_active_for_plan(wf.plan_ids[i]);
      if (db1_execution_plan_cancel_by_id(wf.plan_ids[i], reason) > 0)
      {
         printf("  Cancelled plan #%d\n", wf.plan_ids[i]);
         cancelled++;
      }
   }
   for (int i = 0; i < wf.active_jobs; i++)
   {
      if (db1_agent_job_cancel_by_id(wf.job_ids[i], reason) > 0)
      {
         printf("  Cancelled job #%d\n", wf.job_ids[i]);
         cancelled++;
      }
   }
   for (int i = 0; i < wf.active_delegations; i++)
   {
      if (db1_delegation_spawn_cancel_by_id(wf.delegation_ids[i]) > 0)
      {
         printf("  Cancelled delegation #%d\n", wf.delegation_ids[i]);
         cancelled++;
      }
   }

   printf("aimee: cancel: %d workflow%s cancelled.\n", cancelled, cancelled == 1 ? "" : "s");
}

static void cancel_plan_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *id_str = opt_pos(&opts, 0);
   if (!id_str)
   {
      fprintf(stderr, "Usage: aimee cancel plan <id>\n");
      return;
   }

   int plan_id = atoi(id_str);
   const char *reason = opt_get(&opts, "reason");
   if (!reason)
      reason = "user cancel";

   (void)db1_plan_step_cancel_active_for_plan(plan_id);
   int changed = db1_execution_plan_cancel_by_id(plan_id, reason);
   if (changed > 0)
      printf("Cancelled plan #%d\n", plan_id);
   else
      printf("No active plan with id %d found.\n", plan_id);
}

static void cancel_job_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *id_str = opt_pos(&opts, 0);
   if (!id_str)
   {
      fprintf(stderr, "Usage: aimee cancel job <id>\n");
      return;
   }

   int job_id = atoi(id_str);
   const char *reason = opt_get(&opts, "reason");
   if (!reason)
      reason = "user cancel";

   int changed = db1_agent_job_cancel_by_id(job_id, reason);
   if (changed > 0)
      printf("Cancelled job #%d\n", job_id);
   else
      printf("No active job with id %d found.\n", job_id);
}

static void cancel_delegation_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *id_str = opt_pos(&opts, 0);
   if (!id_str)
   {
      fprintf(stderr, "Usage: aimee cancel delegation <id>\n");
      return;
   }

   int deleg_id = atoi(id_str);
   /* Cascade through children — orphaning workers wastes tokens and
    * the per-session spawn budget. */
   int changed = db1_delegation_spawn_cancel_recursive(deleg_id);
   if (changed > 0)
      printf("Cancelled delegation #%d (and %d descendant%s)\n", deleg_id, changed - 1,
             changed - 1 == 1 ? "" : "s");
   else if (changed == 0)
      printf("No active delegation with id %d found.\n", deleg_id);
   else
      printf("Error cancelling delegation #%d.\n", deleg_id);
}

/* ---- Subcommand table ---- */

static const subcmd_t cancel_subcmds[] = {
    {"all", "Cancel all active workflows (default)", cancel_all},
    {"plan", "Cancel a specific execution plan", cancel_plan_cmd},
    {"job", "Cancel a specific agent job", cancel_job_cmd},
    {"delegation", "Cancel a specific delegation", cancel_delegation_cmd},
    {NULL, NULL, NULL},
};

const subcmd_t *get_cancel_subcmds(void)
{
   return cancel_subcmds;
}

void cmd_cancel(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_require_db1("cannot initialize DB1");

   /* No subcommand or flags starting with -- -> default to cancel_all */
   if (argc < 1 || (argv[0][0] == '-'))
   {
      cancel_all(ctx, argc, argv);
      return;
   }

   /* Check if first arg is a known subcommand */
   for (int i = 0; cancel_subcmds[i].name; i++)
   {
      if (strcmp(argv[0], cancel_subcmds[i].name) == 0)
      {
         subcmd_dispatch(cancel_subcmds, argv[0], ctx, argc - 1, argv + 1);
         return;
      }
   }

   /* Unknown subcommand -- treat as cancel all with remaining args */
   cancel_all(ctx, argc, argv);
}
