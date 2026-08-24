/* cmd_hooks.c: Windows implementations for cmd_hooks platform operations. */
#include "aimee.h"
#include "config.h"
#include "db1_client/db1.h"
#include "kb_client.h"
#include "cmd_hooks_platform.h"
#include "lifecycle.h"
#include <stdio.h>
#include <string.h>

int platform_hooks_stdin_ready(void)
{
   /* Windows best-effort: assume stdin is ready (it is piped, not a console). */
   return 1;
}

static void mark_main_head_indexed(const char *project, const char *head)
{
   if (!project || !project[0] || !head || !head[0])
      return;
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/index-main-%s.head", config_output_dir(), project);
   FILE *f = fopen(path, "w");
   if (!f)
      return;
   fputs(head, f);
   fputc('\n', f);
   fclose(f);
}

void platform_hooks_background_reindex(char idx_names[][128], char idx_roots[][MAX_PATH_LEN],
                                       char idx_heads[][64], int idx_count)
{
   /* No fork on Windows: send scan requests synchronously. kb's
    * coordinator handles cooldown/serialization. */
   for (int i = 0; i < idx_count; i++)
   {
      kb_client_index_scan_result_t res;
      memset(&res, 0, sizeof(res));
      int rc = kb_client_index_scan(idx_names[i], idx_roots[i], 1, &res);
      if (rc == 0 && !res.skipped)
         mark_main_head_indexed(idx_names[i], idx_heads[i]);
   }
}

void platform_hooks_background_cleanup(void)
{
   /* Always synchronous here. The store is a module now, so there is no local
    * connection to open or shut: either it can be reached or the prune waits
    * for a run when it can. */
   if (db1_store_ready())
      prune_stale_sessions();
}
