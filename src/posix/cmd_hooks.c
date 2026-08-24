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

void platform_hooks_background_cleanup(void)
{
   /* Synchronous, and no longer forked.
    *
    * This used to double-fork so session startup was not held up by cleanup,
    * and reopened DB1 in the child because a SQLite connection does not
    * survive fork(). Neither half of that survives the store becoming a
    * module: there is no connection to reopen, and the bus client the child
    * would inherit is a socket with a mutex and possibly a request in flight
    * -- state a forked child cannot use and cannot repair.
    *
    * What it replaced the local scan with is a handful of IPC round trips, so
    * the reason to get off the calling thread is much weaker than the reason
    * not to use a bus client across a fork. Windows has always run this
    * synchronously for the same practical reason. */
   if (db1_store_ready())
      prune_stale_sessions();
}
