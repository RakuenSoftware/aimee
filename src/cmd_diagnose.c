/* cmd_diagnose.c: evidence-driven diagnosis CLI.
 *
 * Subcommands: start, observe, hypothesize, evidence, probe, status, list, conclude, abandon,
 *              investigate. */
#include "aimee.h"
#include "db1_client/db1.h"
#include "commands.h"
#include "platform_process.h"
#include "cJSON.h"
#ifndef AIMEE_WINDOWS
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int parse_rank(const char *s)
{
   if (!s)
      return DIAG_RANK_CODE;
   if (strcmp(s, "direct") == 0 || strcmp(s, "1") == 0)
      return DIAG_RANK_DIRECT;
   if (strcmp(s, "log") == 0 || strcmp(s, "metric") == 0 || strcmp(s, "2") == 0)
      return DIAG_RANK_LOG;
   if (strcmp(s, "code") == 0 || strcmp(s, "3") == 0)
      return DIAG_RANK_CODE;
   if (strcmp(s, "speculation") == 0 || strcmp(s, "4") == 0)
      return DIAG_RANK_SPECULATION;
   return DIAG_RANK_CODE;
}

static void emit_id(app_ctx_t *ctx, int id, const char *key)
{
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", "ok");
      cJSON_AddNumberToObject(j, key, id);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("%s: %d\n", key, id);
   }
}

static void cmd_diagnose_start(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("diagnose start requires a symptom");
   int id = db1_diagnose_start(argv[0]);
   if (id < 0)
      fatal("failed to start diagnosis");
   emit_id(ctx, id, "diagnosis_id");
}

static void cmd_diagnose_observe(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
      fatal("diagnose observe <id> <content> [source]");
   int diag_id = atoi(argv[0]);
   const char *src = argc >= 3 ? argv[2] : "";
   int id = db1_diagnose_add_observation(diag_id, argv[1], src);
   if (id < 0)
      fatal("failed to add observation");
   emit_id(ctx, id, "item_id");
}

static void cmd_diagnose_hypothesize(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
      fatal("diagnose hypothesize <id> <content>");
   int diag_id = atoi(argv[0]);
   int id = db1_diagnose_add_hypothesis(diag_id, argv[1]);
   if (id < 0)
      fatal("failed to add hypothesis");
   emit_id(ctx, id, "item_id");
}

static void cmd_diagnose_evidence(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 4)
      fatal("diagnose evidence <diag_id> <hypothesis_id> <for|against> <content> [rank] [source]");
   int diag_id = atoi(argv[0]);
   int hyp_id = atoi(argv[1]);
   const char *kind_short = argv[2];
   const char *content = argv[3];
   int rank = argc >= 5 ? parse_rank(argv[4]) : DIAG_RANK_CODE;
   const char *source = argc >= 6 ? argv[5] : "";
   const char *kind;
   if (strcmp(kind_short, "for") == 0 || strcmp(kind_short, "+") == 0)
      kind = "evidence_for";
   else if (strcmp(kind_short, "against") == 0 || strcmp(kind_short, "-") == 0)
      kind = "evidence_against";
   else
      fatal("evidence kind must be 'for' or 'against'");
   int id = db1_diagnose_add_evidence(diag_id, hyp_id, kind, content, source, rank);
   if (id < 0)
      fatal("failed to add evidence");
   emit_id(ctx, id, "item_id");
}

static void cmd_diagnose_probe(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 3)
      fatal("diagnose probe <diag_id> <hypothesis_id> <content>");
   int id = db1_diagnose_add_probe(atoi(argv[0]), atoi(argv[1]), argv[2]);
   if (id < 0)
      fatal("failed to add probe");
   emit_id(ctx, id, "item_id");
}

static void cmd_diagnose_status(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("diagnose status <id>");
   int diag_id = atoi(argv[0]);
   if (ctx->json_output)
   {
      char *json = db1_diagnose_json_full(diag_id);
      if (!json)
         fatal("diagnosis %d not found", diag_id);
      cJSON *j = cJSON_Parse(json);
      free(json);
      if (!j)
         fatal("internal: could not parse diagnosis JSON");
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      char *report = db1_diagnose_render_status(diag_id);
      if (!report)
         fatal("diagnosis %d not found", diag_id);
      fputs(report, stdout);
      free(report);
   }
}

static void cmd_diagnose_list(app_ctx_t *ctx, int argc, char **argv)
{
   if (ctx->json_output)
   {
      char *json = db1_diagnose_json_list();
      cJSON *j = cJSON_Parse(json ? json : "[]");
      free(json);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      diagnosis_t rows[128];
      int n = db1_diagnose_list(rows, 128);
      if (n == 0)
         printf("(no diagnoses)\n");
      for (int i = 0; i < n; i++)
         printf("#%d [%s] %s\n", rows[i].id, rows[i].status, rows[i].symptom);
   }
}

static void cmd_diagnose_conclude(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
      fatal("diagnose conclude <id> <conclusion> [confidence]");
   int diag_id = atoi(argv[0]);
   double confidence = argc >= 3 ? atof(argv[2]) : 0.5;
   if (db1_diagnose_conclude(diag_id, argv[1], confidence) != 0)
      fatal("failed to conclude diagnosis %d (already concluded?)", diag_id);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void cmd_diagnose_abandon(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("diagnose abandon <id>");
   if (db1_diagnose_abandon(atoi(argv[0])) != 0)
      fatal("failed to abandon diagnosis");
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void cmd_diagnose_investigate(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   /* aimee diagnose investigate <diag_id> [--parallel [N]] */
   if (argc < 1)
      fatal("diagnose investigate <diag_id> [--parallel [N]]");

   int diag_id = atoi(argv[0]);
   int parallel = 0;
   int max_parallel = 3;
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--parallel") == 0)
      {
         parallel = 1;
         if (i + 1 < argc && argv[i + 1][0] != '-')
         {
            int n = atoi(argv[++i]);
            if (n > 0)
               max_parallel = n;
         }
      }
   }

   /* Verify the diagnosis exists. */
   diagnosis_t diag;
   if (db1_diagnose_get(diag_id, &diag) != 0)
      fatal("diagnosis %d not found", diag_id);

   /* Load hypotheses. */
   diagnosis_item_t hyps[64];
   int hyp_count = db1_diagnose_list_hypotheses(diag_id, hyps, 64);
   if (hyp_count == 0)
      fatal("diagnosis %d has no hypotheses to investigate", diag_id);

   int cap = hyp_count;
   if (parallel && cap > max_parallel)
      cap = max_parallel;

   /* Locate the aimee binary. */
   char aimee_bin[MAX_PATH_LEN];
   if (platform_get_exe_path(aimee_bin, sizeof(aimee_bin)) != 0)
      snprintf(aimee_bin, sizeof(aimee_bin), "aimee");

   printf("Dispatching %d delegate investigator(s) for diagnosis #%d: %s\n", cap, diag_id,
          diag.symptom);
   if (parallel)
      printf("Running in parallel (max %d concurrent).\n", max_parallel);

   int n_pids = 0;
#ifndef AIMEE_WINDOWS
   pid_t pids[64];
#endif

   for (int i = 0; i < cap; i++)
   {
      char prompt[1536];
      snprintf(prompt, sizeof(prompt),
               "You are investigating hypothesis H%d for diagnosis #%d.\n"
               "Symptom: %s\n"
               "Hypothesis: %s\n\n"
               "Your task:\n"
               "1. Investigate whether this hypothesis is supported by evidence.\n"
               "2. For each finding, use the diagnose_evidence MCP tool to record it:\n"
               "   - diagnosis_id: %d, hypothesis_id: %d\n"
               "   - kind: 'for' or 'against', content: your finding, rank: "
               "direct/log/code/speculation\n"
               "3. If you find a discriminating probe that would help confirm or refute this\n"
               "   hypothesis, record it with the diagnose_probe MCP tool.\n"
               "4. End with a brief summary of your findings.\n"
               "Focus on concrete evidence. Do not speculate without basis.",
               i + 1, diag_id, diag.symptom, hyps[i].content, diag_id, hyps[i].id);

      if (!parallel)
      {
         /* Serial: run each delegate synchronously. */
         char cmd[2048];
         snprintf(cmd, sizeof(cmd), "\"%s\" delegate investigator --persona engineer %s", aimee_bin,
                  prompt);
         printf("\n[H%d] Investigating: %s\n", i + 1, hyps[i].content);
         fflush(stdout);
         (void)system(cmd);
      }
      else
      {
#ifdef AIMEE_WINDOWS
         char cmd[2048];
         snprintf(cmd, sizeof(cmd), "\"%s\" delegate investigator --persona engineer %s", aimee_bin,
                  prompt);
         printf("\n[H%d] Investigating: %s\n", i + 1, hyps[i].content);
         fflush(stdout);
         (void)system(cmd);
#else
         /* Parallel: fork a child for each delegate. */
         pid_t pid = fork();
         if (pid < 0)
         {
            fprintf(stderr, "warn: fork failed for H%d, skipping\n", i + 1);
            continue;
         }
         if (pid == 0)
         {
            /* Child: exec aimee delegate. */
            execlp(aimee_bin, "aimee", "delegate", "investigator", "--persona", "engineer", prompt,
                   (char *)NULL);
            _exit(127);
         }
         pids[n_pids++] = pid;
         printf("[H%d] Dispatched delegate (pid %d): %s\n", i + 1, (int)pid, hyps[i].content);
#endif
      }
   }

   /* Wait for all parallel delegates. */
#ifndef AIMEE_WINDOWS
   if (parallel && n_pids > 0)
   {
      printf("Waiting for %d delegate(s) to complete...\n", n_pids);
      fflush(stdout);
      for (int i = 0; i < n_pids; i++)
      {
         int wstatus = 0;
         waitpid(pids[i], &wstatus, 0);
      }
      printf("All delegates complete.\n");
   }
#else
   (void)n_pids;
#endif

   /* Reopen DB and show updated status. */
   if (db1_store_ready())
   {
      printf("\n--- Updated Diagnosis Status ---\n");
      char *report = db1_diagnose_render_status(diag_id);
      if (report)
      {
         fputs(report, stdout);
         free(report);
      }
   }
}

static const subcmd_t cmd_diagnose_subs[] = {
    {"start", "start a diagnosis from a symptom", cmd_diagnose_start},
    {"observe", "record an observation", cmd_diagnose_observe},
    {"hypothesize", "add a hypothesis", cmd_diagnose_hypothesize},
    {"evidence", "add evidence for/against a hypothesis", cmd_diagnose_evidence},
    {"probe", "record a discriminating probe", cmd_diagnose_probe},
    {"status", "show a diagnosis", cmd_diagnose_status},
    {"list", "list diagnoses", cmd_diagnose_list},
    {"conclude", "conclude a diagnosis", cmd_diagnose_conclude},
    {"abandon", "abandon a diagnosis", cmd_diagnose_abandon},
    {"investigate", "dispatch delegate investigators", cmd_diagnose_investigate},
    {NULL, NULL, NULL},
};

void cmd_diagnose(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("diagnose requires a subcommand: start, observe, hypothesize, evidence, probe, status, "
            "list, conclude, abandon, investigate");

   const char *sub = argv[0];
   argc--;
   argv++;

   cmd_require_db1("cannot initialize DB1");

   if (subcmd_dispatch(cmd_diagnose_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown diagnose subcommand: %s", sub);
}
