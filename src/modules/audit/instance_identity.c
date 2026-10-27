/* instance_identity.c: the aimee instance roles and their first-boot identity.
 *
 * Moved out of the ring's runtime when the ring became its own repository
 * (2026-10-27): which daemon an instance is, and the latch the Go identity
 * package writes on first boot, are aimee's deployment, not the ring's. */
#define _GNU_SOURCE
#include <aimee/audit/instance_identity.h>

#include <ouroboros/runtime.h> /* ouro_runtime_monotonic_ns */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

const uint32_t *aimee_instance_refused_role_refs(aimee_instance_role_t role, size_t *count)
{
   static const uint32_t refuse_kb[] = {AIMEE_KB_ROLE_REF};
   static const uint32_t refuse_server[] = {AIMEE_SERVER_ROLE_REF};
   static const uint32_t refuse_both[] = {AIMEE_SERVER_ROLE_REF, AIMEE_KB_ROLE_REF};
   switch (role)
   {
   case AIMEE_INSTANCE_SERVER:
      *count = 1;
      return refuse_kb;
   case AIMEE_INSTANCE_KB:
      *count = 1;
      return refuse_server;
   default:
      *count = 2;
      return refuse_both;
   }
}

/* The latch is canonical JSON emitted by the Go identity package. Parse this
 * small fixed record without introducing a JSON/storage dependency into core.
 * Anything except the exact versioned encoding is rejected, never rewritten. */
static int instance_latch_read(const char *path, const char *role)
{
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return errno == ENOENT ? 1 : -1;
   struct stat st;
   char wire[128] = {0}, prefix[64];
   ssize_t n = read(fd, wire, sizeof(wire));
   int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0444 &&
            st.st_uid == geteuid();
   close(fd);
   int prefix_len =
       snprintf(prefix, sizeof(prefix), "{\"version\":1,\"role\":\"%s\",\"id\":\"", role);
   if (!ok || prefix_len <= 0 || n != prefix_len + 39 ||
       memcmp(wire, prefix, (size_t)prefix_len) != 0 ||
       memcmp(wire + prefix_len + 36, "\"}\n", 3) != 0)
      return -1;
   for (int i = 0; i < 36; i++)
   {
      char c = wire[prefix_len + i];
      if (i == 8 || i == 13 || i == 18 || i == 23)
      {
         if (c != '-')
            return -1;
      }
      else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return -1;
   }
   return 0;
}

int aimee_instance_ensure_identity(const char *home, aimee_instance_role_t role,
                                   const char *runtime_binary)
{
   const char *name = role == AIMEE_INSTANCE_SERVER ? "server"
                      : role == AIMEE_INSTANCE_KB   ? "kb"
                                                    : NULL;
   char path[PATH_MAX], installed[PATH_MAX], argv0[64];
   if (!name || !home || home[0] != '/' ||
       (size_t)snprintf(path, sizeof(path), "%s/instance-identity.json", home) >= sizeof(path))
      return -1;
   int status = instance_latch_read(path, name);
   if (status != 1)
      return status;
   snprintf(argv0, sizeof(argv0), "aimee-module-%s", name);
   snprintf(installed, sizeof(installed), "/usr/local/libexec/aimee-modules/%s", argv0);
   if (!runtime_binary || !runtime_binary[0])
      runtime_binary = getenv("AIMEE_TEST_MODULE_BIN");
   const char *binary = runtime_binary && runtime_binary[0] ? runtime_binary : installed;
   if (binary[0] != '/')
      return -1;
   char *args[] = {argv0, "__aimee_instance_bootstrap", (char *)home, NULL};
   posix_spawn_file_actions_t actions;
   if (posix_spawn_file_actions_init(&actions) != 0)
      return -1;
   if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) != 0)
   {
      posix_spawn_file_actions_destroy(&actions);
      return -1;
   }
   extern char **environ;
   pid_t child;
   int rc = posix_spawn(&child, binary, &actions, NULL, args, environ);
   posix_spawn_file_actions_destroy(&actions);
   if (rc != 0)
      return -1;
   uint64_t until = ouro_runtime_monotonic_ns() + 10ULL * 1000000000ULL;
   for (;;)
   {
      pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child)
         break;
      if ((waited < 0 && errno != EINTR) || ouro_runtime_monotonic_ns() >= until)
      {
         kill(child, SIGKILL);
         while (waitpid(child, &status, 0) < 0 && errno == EINTR)
         {
         }
         return -1;
      }
      usleep(10000);
   }
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      return -1;
   return instance_latch_read(path, name);
}
