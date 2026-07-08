/* cmd_ensemble.c: multi-agent ensemble (templated multi-agent session) commands */
#include "aimee.h"
#include "commands.h"
#include "config.h"
#include "util.h"
#include "db1.h"
#include "cJSON.h"
#include <ctype.h>
#include <unistd.h>

static cJSON *parse_assignments(int argc, char **argv, char *err, size_t errlen)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      snprintf(err, errlen, "out of memory");
      return NULL;
   }

   for (int i = 0; i < argc; i++)
   {
      const char *spec = NULL;
      if (strcmp(argv[i], "--assign") == 0 && i + 1 < argc)
         spec = argv[++i];
      else if (strncmp(argv[i], "--assign=", 9) == 0)
         spec = argv[i] + 9;
      else
         continue;

      const char *eq = strchr(spec, '=');
      if (!eq || eq == spec || !eq[1])
      {
         snprintf(err, errlen, "invalid --assign '%s' (expected role=name[,name])", spec);
         cJSON_Delete(root);
         return NULL;
      }

      char role[ENSEMBLE_ROLE_NAME_MAX];
      size_t role_len = (size_t)(eq - spec);
      if (role_len >= sizeof(role))
         role_len = sizeof(role) - 1;
      memcpy(role, spec, role_len);
      role[role_len] = '\0';

      cJSON *arr = cJSON_CreateArray();
      if (!arr)
      {
         snprintf(err, errlen, "out of memory");
         cJSON_Delete(root);
         return NULL;
      }

      char *names = safe_strdup(eq + 1);
      char *save = NULL;
      for (char *tok = strtok_r(names, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
      {
         while (*tok && isspace((unsigned char)*tok))
            tok++;
         char *end = tok + strlen(tok);
         while (end > tok && isspace((unsigned char)end[-1]))
            *--end = '\0';
         if (*tok)
            cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
      }
      free(names);

      if (cJSON_GetArraySize(arr) == 0)
      {
         snprintf(err, errlen, "assignment for role '%s' is empty", role);
         cJSON_Delete(arr);
         cJSON_Delete(root);
         return NULL;
      }

      cJSON_DeleteItemFromObjectCaseSensitive(root, role);
      cJSON_AddItemToObject(root, role, arr);
   }

   return root;
}

static void print_ensemble_status(const ensemble_info_t *info, const char *prompt,
                                  const char *context)
{
   printf("ensemble #%d\n", info->id);
   printf("  template: %s\n", info->template_name);
   printf("  channel:  %s\n", info->channel);
   printf("  status:   %s\n", info->status);
   if (info->phase_name[0])
      printf("  phase:    %s (%d/%d)\n", info->phase_name, info->current_phase + 1,
             info->phase_count);
   if (info->turns_in_phase > 0)
      printf("  turn:     %d/%d\n", info->current_turn + 1, info->turns_in_phase);
   if (info->expected_agent[0])
      printf("  expected: %s [%s]\n", info->expected_agent, info->expected_role);
   if (info->paused_reason[0])
      printf("  paused:   %s\n", info->paused_reason);
   if (context && context[0])
      printf("\nrecent context:\n%s", context);
   if (prompt && prompt[0])
      printf("\nnext prompt:\n%s\n", prompt);
}

void session_subcmd_start(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee ensemble start <template> [--channel NAME] [--assign "
                      "role=agent[,agent]]\n");
      return;
   }

   const char *template_name = argv[0];
   const char *channel = "general";
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc)
         channel = argv[++i];
      else if (strncmp(argv[i], "--channel=", 10) == 0)
         channel = argv[i] + 10;
   }

   char err[256] = "";
   cJSON *assignments = parse_assignments(argc - 1, argv + 1, err, sizeof(err));
   if (!assignments)
   {
      fprintf(stderr, "ensemble start: %s\n", err[0] ? err : "failed to parse assignments");
      return;
   }

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   int id = 0;
   if (db1_ensemble_create(cwd, template_name, channel, assignments, &id, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "ensemble start: %s\n", err);
      cJSON_Delete(assignments);
      return;
   }
   cJSON_Delete(assignments);

   ensemble_info_t info;
   char *prompt = NULL;
   char *context = NULL;
   if (db1_ensemble_get(id, &info, &prompt, &context, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "ensemble start: %s\n", err);
      free(prompt);
      free(context);
      return;
   }

   print_ensemble_status(&info, prompt, context);
   free(prompt);
   free(context);
}

void session_subcmd_status(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee ensemble status <id>\n");
      return;
   }

   char err[256] = "";
   ensemble_info_t info;
   char *prompt = NULL;
   char *context = NULL;
   if (db1_ensemble_get(atoi(argv[0]), &info, &prompt, &context, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "ensemble status: %s\n", err);
      free(prompt);
      free(context);
      return;
   }

   print_ensemble_status(&info, prompt, context);
   free(prompt);
   free(context);
}

void session_subcmd_pause(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee ensemble pause <id> [reason]\n");
      return;
   }

   char reason[64];
   snprintf(reason, sizeof(reason), "%s", (argc >= 2 && argv[1][0]) ? argv[1] : "manual");
   char err[256] = "";
   if (db1_ensemble_pause(atoi(argv[0]), reason, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "ensemble pause: %s\n", err);
      return;
   }
   printf("ensemble #%d paused (%s)\n", atoi(argv[0]), reason);
}

void session_subcmd_advance(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee ensemble advance <id> --speaker NAME --message TEXT\n");
      return;
   }

   const char *speaker = NULL;
   const char *message = NULL;
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--speaker") == 0 && i + 1 < argc)
         speaker = argv[++i];
      else if (strcmp(argv[i], "--message") == 0 && i + 1 < argc)
         message = argv[++i];
      else if (strncmp(argv[i], "--speaker=", 10) == 0)
         speaker = argv[i] + 10;
      else if (strncmp(argv[i], "--message=", 10) == 0)
         message = argv[i] + 10;
   }

   char err[256] = "";
   ensemble_info_t info;
   char *prompt = NULL;
   if (db1_ensemble_advance(atoi(argv[0]), speaker, message ? message : "", &info, &prompt, err,
                            sizeof(err)) != 0)
   {
      fprintf(stderr, "ensemble advance: %s\n", err);
      free(prompt);
      return;
   }

   print_ensemble_status(&info, prompt, NULL);
   free(prompt);
}

/* `aimee ensemble <sub>` — the canonical verb for multi-agent ensemble sessions.
 * The same handlers remain reachable under `aimee session <sub>` as back-compat
 * aliases (see session_subcmds in cmd_infra.c). */
static const subcmd_t ensemble_subcmds[] = {
    {"start", "Start a templated multi-agent ensemble (code-review, debate, ...)",
     session_subcmd_start},
    {"status", "Show ensemble status by id", session_subcmd_status},
    {"pause", "Pause an ensemble [reason]", session_subcmd_pause},
    {"advance", "Advance an ensemble with --speaker/--message", session_subcmd_advance},
    {NULL, NULL, NULL},
};

const subcmd_t *get_ensemble_subcmds(void)
{
   return ensemble_subcmds;
}

void cmd_ensemble(app_ctx_t *ctx, int argc, char **argv)
{
   config_t db1_cfg;
   config_load(&db1_cfg);
   if (db1_init(db1_cfg.db1_path) != 0)
      fatal("ensemble: could not initialize DB1");

   const char *sub = (argc > 0) ? argv[0] : NULL;
   if (argc > 0)
   {
      argc--;
      argv++;
   }

   if (subcmd_dispatch(ensemble_subcmds, sub, ctx, argc, argv) != 0)
      subcmd_usage("ensemble", ensemble_subcmds);
}
