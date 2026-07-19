/* cmd_auto.c: aimee auto — spec-driven roadmap dispatch loop CLI */
#include "aimee.h"
#include "log.h"
#include "commands.h"
#include "roadmap_auto.h"
#include "cJSON.h"

static void cmd_auto_start(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("auto start requires a roadmap id");
   const char *id = argv[0];
   argc--;
   argv++;

   roadmap_auto_cfg_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--max-verify-retries") == 0 && i + 1 < argc)
         cfg.max_verify_retries = atoi(argv[++i]);
      else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc)
         cfg.budget_ceiling_tokens = atoi(argv[++i]);
      else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
         cfg.soft_timeout_s = atoi(argv[++i]);
      else if (strcmp(argv[i], "--max-iterations") == 0 && i + 1 < argc)
         cfg.max_iterations = atoi(argv[++i]);
      else if (strcmp(argv[i], "--no-slice-gate") == 0)
         cfg.require_slice_discussion = 0;
      else if (strcmp(argv[i], "--headless") == 0)
         cfg.headless = 1;
      else if (strcmp(argv[i], "--max-restarts") == 0 && i + 1 < argc)
      {
         cfg.max_restarts = atoi(argv[++i]);
         cfg.headless = 1; /* --max-restarts implies headless */
      }
      else if (strcmp(argv[i], "--reassess") == 0)
         cfg.run_reassessment = 1;
   }

   char exit_reason[64] = "";
   int rc = 0;
   int restarts = 0;

   for (;;)
   {
      exit_reason[0] = '\0';
      rc = roadmap_auto_run(id, &cfg, exit_reason, sizeof(exit_reason));

      /* Non-headless: single run. */
      if (!cfg.headless)
         break;
      /* Hard error: don't restart. */
      if (rc != 0)
         break;
      /* Explicit stop: honour it. */
      if (strcmp(exit_reason, ROADMAP_AUTO_EXIT_STOPPED) == 0)
         break;
      /* All complete: done, no restart needed. */
      if (strcmp(exit_reason, ROADMAP_AUTO_EXIT_ALL_COMPLETE) == 0)
         break;
      /* Restart ceiling. */
      if (cfg.max_restarts > 0 && restarts >= cfg.max_restarts)
      {
         fprintf(stderr, "auto: headless: max_restarts=%d reached — stopping\n", cfg.max_restarts);
         break;
      }
      restarts++;
      fprintf(stderr, "auto: headless: restart %d (exit=%s)\n", restarts,
              exit_reason[0] ? exit_reason : "paused");
   }

   if (ctx->json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "roadmap_id", id);
      cJSON_AddStringToObject(o, "exit_reason", exit_reason);
      cJSON_AddNumberToObject(o, "restarts", restarts);
      cJSON_AddBoolToObject(o, "ok", rc == 0);
      emit_json_ctx(o, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (rc == 0)
         printf("auto: exited — %s\n", exit_reason[0] ? exit_reason : "done");
      else
         fprintf(stderr, "auto: error running roadmap %s\n", id);
   }
   if (rc != 0)
      exit(1);
}

static void cmd_auto_pause(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("auto pause requires a roadmap id");
   if (roadmap_auto_pause(argv[0]) != 0)
      fatal("auto pause: failed to pause roadmap %s", argv[0]);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("paused %s\n", argv[0]);
}

static void cmd_auto_resume(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("auto resume requires a roadmap id");
   const char *id = argv[0];
   char exit_reason[64] = "";
   int rc = roadmap_auto_resume(id, NULL, exit_reason, sizeof(exit_reason));
   if (ctx->json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "roadmap_id", id);
      cJSON_AddStringToObject(o, "exit_reason", exit_reason);
      cJSON_AddBoolToObject(o, "ok", rc == 0);
      emit_json_ctx(o, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("auto: exited — %s\n", exit_reason[0] ? exit_reason : "done");
   }
   if (rc != 0)
      exit(1);
}

static void cmd_auto_stop(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("auto stop requires a roadmap id");
   if (roadmap_auto_stop(argv[0]) != 0)
      fatal("auto stop: failed to stop roadmap %s", argv[0]);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("stopped %s\n", argv[0]);
}

static void cmd_auto_status(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("auto status requires a roadmap id");
   if (roadmap_auto_status(argv[0], ctx->json_output) != 0)
      fatal("auto status: roadmap %s not found", argv[0]);
}

static const subcmd_t cmd_auto_subs[] = {
    {"start", "run a roadmap dispatch loop", cmd_auto_start},
    {"run", "alias for start", cmd_auto_start},
    {"pause", "pause a running roadmap", cmd_auto_pause},
    {"resume", "resume a paused roadmap", cmd_auto_resume},
    {"stop", "stop a running roadmap", cmd_auto_stop},
    {"status", "show a roadmap's dispatch status", cmd_auto_status},
    {NULL, NULL, NULL},
};

void cmd_auto(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("auto requires a subcommand: start, pause, resume, stop, status");

   const char *sub = argv[0];
   argc--;
   argv++;

   if (subcmd_dispatch(cmd_auto_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown auto subcommand: %s (start, pause, resume, stop, status)", sub);
}
