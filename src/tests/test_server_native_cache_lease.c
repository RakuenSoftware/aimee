#include "server_native_cache_lease.h"
#include "platform_test_util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static const char binding[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static char home[4096], directory[4096];
static server_native_cache_lease_t *open_lease(const char *turn, char error[512])
{
   return server_native_cache_lease_open(home, "codex", "session", binding, "actor", turn, error);
}
static void path(char out[8192], const char *name)
{
   assert(snprintf(out, 8192, "%s/%s", directory, name) < 8192);
}
static void wait_exit(pid_t child)
{
   int status;
   assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}
int main(void)
{
   assert(snprintf(home, sizeof home, "%s/native-cache-lease-XXXXXX", platform_tmpdir()) <
          (int)sizeof home);
   assert(mkdtemp(home));
   assert(snprintf(directory, sizeof directory, "%s/browser-codex/session/%s", home, binding) <
          (int)sizeof directory);
   char error[512], marker[8192], lock[8192], cache[8192], replacement[8192];
   path(marker, ".aimee-native-owner.json");
   path(lock, ".aimee-native-owner.lock");
   path(cache, "committed-conversation");
   path(replacement, "replacement");
   server_native_cache_lease_t *lease = open_lease("turn-one", error);
   assert(lease);
   assert(!open_lease("turn-two", error) && strstr(error, "busy"));
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      assert(!open_lease("parallel-context", error) && strstr(error, "busy"));
      _exit(0);
   }
   wait_exit(child);
   assert(!server_native_cache_lease_close(lease, 1));
   assert(access(marker, F_OK) && !access(lock, F_OK));
   int file = open(cache, O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(file >= 0 && write(file, "COMMITTED\n", 10) == 10);
   close(file);

#ifdef __linux__
   // Real owner death releases flock, while an escaped setsid descendant keeps
   // writing SDK cache data. Restart must still refuse the durable marker.
   assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
   int ready[2];
   assert(!pipe(ready));
   child = fork();
   assert(child >= 0);
   if (!child)
   {
      close(ready[0]);
      assert(open_lease("crashed-turn", error));
      pid_t writer = fork();
      assert(writer >= 0);
      if (!writer)
      {
         assert(setsid() >= 0);
         for (int fd = 3; fd < 1024; fd++)
            if (fd != ready[1])
               close(fd); // Like descriptor-native exec: no inherited lease.
         int output = open(cache, O_WRONLY | O_APPEND);
         assert(output >= 0);
         char byte = 'x';
         assert(write(output, &byte, 1) == 1);
         pid_t self = getpid();
         assert(write(ready[1], &self, sizeof self) == sizeof self);
         close(ready[1]);
         for (;;)
         {
            assert(write(output, &byte, 1) == 1);
            usleep(10000);
         }
      }
      close(ready[1]);
      for (;;)
         pause();
   }
   close(ready[1]);
   pid_t writer;
   assert(read(ready[0], &writer, sizeof writer) == sizeof writer);
   close(ready[0]);
   assert(!kill(child, SIGKILL));
   int status;
   assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
   assert(!open_lease("restart", error) && strstr(error, "unresolved prior owner"));
   struct stat before, after;
   assert(!stat(cache, &before));
   usleep(60000);
   assert(!stat(cache, &after) && after.st_size > before.st_size);
   assert(!kill(writer, SIGKILL));
   assert(waitpid(writer, &status, 0) == writer && WIFSIGNALED(status));
   // Even a positively dead test descendant alone is not a production recovery
   // contract. The marker remains refused; only fixture teardown removes it.
   assert(!open_lease("after-writer-exit", error) && strstr(error, "unresolved prior owner"));
   assert(!unlink(marker));
#endif

   lease = open_lease("replacement-turn", error);
   assert(lease);
   file = open(replacement, O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(file >= 0 && write(file, "foreign", 7) == 7);
   close(file);
   assert(!rename(replacement, marker));
   assert(server_native_cache_lease_close(lease, 1) == -1);
   assert(!open_lease("foreign-marker", error) && strstr(error, "unresolved prior owner"));
   assert(!unlink(marker));
   assert(!unlink(lock));
   assert(!symlink(cache, lock));
   assert(!open_lease("unsafe-lock", error));
   assert(!unlink(lock));
   assert(!unlink(cache));
   assert(!rmdir(directory));
   char parent[8192];
   assert(snprintf(parent, sizeof parent, "%s/browser-codex/session", home) < (int)sizeof parent);
   assert(!rmdir(parent));
   assert(snprintf(parent, sizeof parent, "%s/browser-codex", home) < (int)sizeof parent);
   assert(!rmdir(parent));
   assert(!rmdir(home));
   puts("native cache pre-spawn lease, crash marker and escaped-writer refusal passed");
   return 0;
}
