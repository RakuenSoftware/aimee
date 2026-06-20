/* cmd_sweep.c: `aimee sweep` — thin client for the server-side deepening sweep.
 * Drives the dev.sweep method (server-side handle_dev_sweep) and prints its report.
 * Analysis-only: the server proposes + re-grounds seams; it files nothing. */
#include "cJSON.h"
#include "cli_client.h"
#include "commands.h"

#include <stdio.h>

void cmd_sweep(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return;
   cJSON_AddStringToObject(req, "method", "dev.sweep");
   /* optional first arg = indexed project name to scope the walk */
   if (argc > 1 && argv[1] && argv[1][0])
      cJSON_AddStringToObject(req, "project", argv[1]);

   /* The sweep fans out one proposer delegate per area; allow a generous deadline. */
   cJSON *resp = cli_v1_dispatch(req, 600000);
   cJSON_Delete(req);
   if (!resp)
   {
      fprintf(stderr, "sweep: no response (is aimee-server reachable?)\n");
      return;
   }
   char *s = cJSON_Print(resp);
   if (s)
   {
      printf("%s\n", s);
      free(s);
   }
   cJSON_Delete(resp);
}
