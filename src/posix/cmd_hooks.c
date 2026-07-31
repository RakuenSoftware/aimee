/* cmd_hooks.c: POSIX platform implementations for cmd_hooks background operations. */
#include "aimee.h"
#include "db1.h"
#include "config.h"
#include "kb_client.h"
#include "cmd_hooks_platform.h"
#include "lifecycle.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int platform_hooks_stdin_ready(void)
{
   fd_set rfds;
   FD_ZERO(&rfds);
   FD_SET(STDIN_FILENO, &rfds);
   struct timeval tv = {0, 0};
   return select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0 ? 1 : 0;
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

void platform_hooks_background_cleanup(const config_t *cfg)
{
   /* Double-fork: same pattern — intermediate exits immediately, worker
    * runs under init so no zombie is left in the caller. */
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

   /* prune_stale_sessions only needs DB1 directly; all DB2 work goes
    * through kb_client which auto-spawns aimee-kb.  DB1 connections
    * are not fork-safe so reopen here. */
   if (db1_init(config_db1_path()) == 0)
   {
      prune_stale_sessions();
      db1_shutdown();
   }
   _exit(0);
}
