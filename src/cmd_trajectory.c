/* cmd_trajectory.c: trajectory export CLI. */
#include "commands.h"

#include "agent_config.h"
#include "db1.h"
#include "trajectory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trajectory_usage(void)
{
   fprintf(stderr,
           "usage: aimee trajectory export <session_id> [--no-compress] [--max-result-bytes N]\n"
           "       aimee trajectory batch --tasks corpus.jsonl|suite_dir "
           "[--toolset-dist research] [--out dir]\n");
}

static void trajectory_export_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      trajectory_usage();
      return;
   }

   const char *selector = argv[0];
   trajectory_opts_t opts = {.compress = 1, .redact = 1, .max_tool_result_bytes = 512};
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--no-compress") == 0)
         opts.compress = 0;
      else if (strcmp(argv[i], "--max-result-bytes") == 0 && i + 1 < argc)
         opts.max_tool_result_bytes = atoi(argv[++i]);
      else
      {
         trajectory_usage();
         return;
      }
   }
   if (!db1_store_ready())
      fatal("trajectory: could not initialize DB1");

   char *json = NULL;
   if (trajectory_export(selector, &opts, &json) != 0 || !json)
      fatal("trajectory export failed; no matching redacted exportable session");

   printf("%s\n", json);
   free(json);
   (void)ctx;
}

static void trajectory_batch_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   trajectory_batch_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.toolset_dist = "research";
   opts.export_opts.compress = 1;
   opts.export_opts.redact = 1;
   opts.export_opts.max_tool_result_bytes = 512;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--tasks") == 0 && i + 1 < argc)
         opts.tasks_path = argv[++i];
      else if (strcmp(argv[i], "--toolset-dist") == 0 && i + 1 < argc)
         opts.toolset_dist = argv[++i];
      else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
         opts.out_dir = argv[++i];
      else if (strcmp(argv[i], "--no-compress") == 0)
         opts.export_opts.compress = 0;
      else if (strcmp(argv[i], "--max-result-bytes") == 0 && i + 1 < argc)
         opts.export_opts.max_tool_result_bytes = atoi(argv[++i]);
      else
      {
         trajectory_usage();
         return;
      }
   }
   if (!opts.tasks_path || !opts.tasks_path[0])
   {
      trajectory_usage();
      return;
   }
   if (!db1_store_ready())
      fatal("trajectory: could not initialize DB1");
   agent_config_t agents;
   if (agent_load_config(&agents) != 0)
      fatal("trajectory batch failed: could not load agents");

   char *summary = NULL;
   if (trajectory_batch_run(&agents, &opts, &summary) != 0 || !summary)
      fatal("trajectory batch failed");
   printf("%s\n", summary);
   free(summary);
   (void)ctx;
}

static const subcmd_t cmd_trajectory_subs[] = {
    {"export", "export a session trajectory", trajectory_export_cmd},
    {"batch", "run a batch over a task corpus", trajectory_batch_cmd},
    {NULL, NULL, NULL},
};

void cmd_trajectory(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sub = argc > 0 ? argv[0] : NULL;
   if (argc > 0)
   {
      argc--;
      argv++;
   }
   if (!sub || subcmd_dispatch(cmd_trajectory_subs, sub, ctx, argc, argv) != 0)
      trajectory_usage();
}
