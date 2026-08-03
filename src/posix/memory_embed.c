/* posix/memory_embed.c: background embedding via aimee-kb.
 *
 * Split from posix/memory.c so memory unit tests can link the gates
 * without dragging in the kb_client RPC chain. */
#include "aimee.h"
#include "modules/memory/memory_platform.h"
#include "kb_client.h"
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static __thread int g_background_embed_suppressed = 0;

int platform_memory_background_embed_set_suppressed(int suppressed)
{
   int prev = g_background_embed_suppressed;
   g_background_embed_suppressed = suppressed ? 1 : 0;
   return prev;
}

void platform_memory_background_embed(int64_t memory_id, const char *command)
{
   if (g_background_embed_suppressed)
      return;

   /* Double-fork so the kb RPC worker is reparented to init.
    * The intermediate child exits immediately; the caller's waitpid()
    * returns without delay and no zombie is left behind. */
   pid_t pid = fork();
   if (pid < 0)
      return;
   if (pid > 0)
   {
      waitpid(pid, NULL, 0);
      return;
   }

   /* Intermediate child: fork actual worker, then exit. */
   pid_t worker = fork();
   if (worker < 0)
      _exit(1);
   if (worker > 0)
      _exit(0);

   /* Actual worker */
   char *resp = kb_client_memory_embed_json(0, memory_id, NULL, command);
   free(resp);
   _exit(0);
}
