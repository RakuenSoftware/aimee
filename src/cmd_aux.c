/* cmd_aux.c: aimee aux — auxiliary model routing CLI. */
#include "aux_router.h"
#include "aimee.h"
#include "commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void aux_subcmd_test(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: aimee aux test <task> \"<prompt>\" [max_tokens]\n");
      return;
   }
   if (!ctx->cfg || !ctx->cfg->aux_enabled)
   {
      fprintf(stderr, "aux routing is disabled (set auxiliary.enabled: true in aimee.yaml)\n");
      return;
   }
   const char *task = argv[0];
   const char *prompt = argv[1];
   int max_tokens = (argc >= 3) ? atoi(argv[2]) : 0;
   char *result = aux_call(task, prompt, max_tokens);
   if (!result)
   {
      fprintf(stderr, "aux_call failed for task '%s'\n", task);
      return;
   }
   printf("%s\n", result);
   free(result);
}

static void aux_subcmd_config(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc > 0 && strcmp(argv[0], "show") != 0)
   {
      fprintf(stderr, "usage: aimee aux config [show]\n");
      return;
   }
   aux_config_show();
}

static const subcmd_t aux_subcmds[] = {
    {"test", "Execute a single auxiliary task call (test <task> \"<prompt>\")", aux_subcmd_test},
    {"config", "Show resolved aux task->provider/model mapping", aux_subcmd_config},
    {NULL, NULL, NULL},
};

void cmd_aux(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      aux_config_show();
      return;
   }
   subcmd_dispatch(aux_subcmds, argv[0], ctx, argc - 1, argv + 1);
}

const subcmd_t *get_aux_subcmds(void)
{
   return aux_subcmds;
}
