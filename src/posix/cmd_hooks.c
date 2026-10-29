/* cmd_hooks.c: POSIX platform implementations for cmd_hooks background operations. */
#include "aimee.h"
#include "db1_client/db1.h"
#include "config.h"
#include "kb_client.h"
#include "cmd_hooks_platform.h"
#include "lifecycle.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
   /* Double-fork: intermediate child exits immediately so the caller's
    * waitpid() returns at once and the actual worker is reparented to init. */
   pid_t pid = fork();
   if (pid < 0)
      return;
   if (pid > 0)
   {
      waitpid(pid, NULL, 0);
      return;
   }

   pid_t worker = fork();
   if (worker < 0)
      _exit(1);
   if (worker > 0)
      _exit(0);

   for (int i = 0; i < idx_count; i++)
   {
      kb_client_index_scan_result_t res;
      memset(&res, 0, sizeof(res));
      int rc = kb_client_index_scan(idx_names[i], idx_roots[i], 1, &res);
      if (rc == 0 && !res.skipped)
         mark_main_head_indexed(idx_names[i], idx_heads[i]);
   }
   _exit(0);
}
