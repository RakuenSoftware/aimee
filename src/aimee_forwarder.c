/* aimee-forwarder: a tiny TCP<->UNIX-socket bridge that runs INSIDE a delegate's
 * `--network none` sandbox container.
 *
 * The sandbox has only loopback plus a bind-mounted aimee UNIX socket. Package
 * managers speak to an `http_proxy=http://127.0.0.1:PORT`, so this listens on
 * 127.0.0.1:PORT and splices each connection to the aimee UDS, where aimee-server
 * runs the package forward-proxy. aimee performs every real fetch; the delegate never
 * holds an outside socket.
 *
 * Statically linked, libc-only, no config files. Fork-per-connection. Env:
 *   AIMEE_FORWARDER_PORT  (default 3129)
 *   AIMEE_FORWARDER_SOCK  (default /run/aimee/aimee-http.sock)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "headers/scoped_file_io.h"
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define DEFAULT_PORT 3129
#define DEFAULT_SOCK "/run/aimee/aimee-http.sock"
#define MAX_CHILDREN 256 /* cap concurrent tunnels: bound PID/FD/UDS-slot exhaustion */

static volatile sig_atomic_t g_children = 0;

static void reap_children(int sig)
{
   (void)sig;
   int saved = errno;
   while (waitpid(-1, NULL, WNOHANG) > 0)
      if (g_children > 0)
         g_children--;
   errno = saved;
}

static int write_all(int fd, const char *p, size_t n)
{
   size_t off = 0;
   while (off < n)
   {
      ssize_t w = write(fd, p + off, n - off);
      if (w < 0 && errno == EINTR)
         continue;
      if (w <= 0)
         return -1;
      off += (size_t)w;
   }
   return 0;
}

/* Bidirectional splice with a generous idle timeout. */
static void pump(int a, int b)
{
   struct pollfd pf[2];
   pf[0].fd = a;
   pf[1].fd = b;
   char buf[65536];
   for (;;)
   {
      pf[0].events = pf[1].events = POLLIN;
      pf[0].revents = pf[1].revents = 0;
      if (poll(pf, 2, 300000) <= 0)
         return;
      for (int i = 0; i < 2; i++)
      {
         if (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
         {
            int src = i == 0 ? a : b, dst = i == 0 ? b : a;
            ssize_t n = read(src, buf, sizeof(buf));
            if (n <= 0)
               return;
            if (write_all(dst, buf, (size_t)n) != 0)
               return;
         }
      }
   }
}

static int connect_uds(const char *path)
{
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
   {
      close(fd);
      return -1;
   }
   return fd;
}

/* Private container resource transport. The verified container starts only the
 * immutable idle guard. This file owner reaches DUMPABLE=0/READY before any
 * external agent or model-controlled shell starts; its private stdio is never
 * inherited by those processes. Callers retain policy and source admission. */
static int owner_nondumpable(void)
{
#if defined(__linux__)
   return prctl(PR_SET_DUMPABLE, 0) || prctl(PR_GET_DUMPABLE) != 0 ? -1 : 0;
#else
   return -1;
#endif
}
static int read_exact(int fd, char *bytes, size_t length)
{
   size_t offset = 0;
   while (offset < length)
   {
      ssize_t n = read(fd, bytes + offset, length - offset);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return -1;
      offset += (size_t)n;
   }
   return 0;
}
static int read_line(int fd, char *line, size_t capacity)
{
   for (size_t i = 0; i + 1 < capacity; i++)
   {
      ssize_t n = read(fd, line + i, 1);
      if (n < 0 && errno == EINTR)
      {
         i--;
         continue;
      }
      if (n <= 0)
         return !n && !i ? 0 : -1;
      if (line[i] == '\n')
      {
         line[i] = 0;
         return 1;
      }
   }
   return -1;
}
static char *resolve_program(const char *command)
{
   if (!command || !command[0] || command[0] == '-')
      return NULL;
   const char *directories[] = {"/usr/local/bin", "/usr/bin", "/bin"};
   for (size_t i = 0; i < 3; i++)
   {
      char path[4096];
      int n = strchr(command, '/') ? snprintf(path, sizeof path, "%s", command)
                                   : snprintf(path, sizeof path, "%s/%s", directories[i], command);
      if (n < 0 || (size_t)n >= sizeof path)
         return NULL;
      char *resolved = realpath(path, NULL);
      struct stat status;
      if (resolved && !stat(resolved, &status) && S_ISREG(status.st_mode) &&
          (status.st_mode & 0111) && !access(resolved, X_OK))
         return resolved;
      free(resolved);
      if (strchr(command, '/'))
         break;
   }
   return NULL;
}
static int file_serve(const char *root_path, const char *selected_command)
{
   if (owner_nondumpable())
      return 1;
   int root = open(root_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (root < 0)
      return 1;
   if (fchdir(root))
   {
      close(root);
      return 1;
   }
   char *command = resolve_program(selected_command), *mcp = resolve_program("aimee");
   char home[] = P_tmpdir "/aimee-acp-owner-XXXXXX", ready[128];
   if (!command || !mkdtemp(home))
   {
      free(command);
      free(mcp);
      close(root);
      return 1;
   }
   int n = snprintf(ready, sizeof ready, "raw-owner-v1 ready %zu %zu %zu\n", strlen(command),
                    strlen(home), mcp ? strlen(mcp) : 0);
   int failed = n < 0 || (size_t)n >= sizeof ready || write_all(STDOUT_FILENO, ready, (size_t)n) ||
                write_all(STDOUT_FILENO, command, strlen(command)) ||
                write_all(STDOUT_FILENO, home, strlen(home)) ||
                (mcp && write_all(STDOUT_FILENO, mcp, strlen(mcp)));
   free(command);
   free(mcp);
   if (failed)
   {
      rmdir(home);
      close(root);
      return 1;
   }
   int rc = 0;
   for (unsigned count = 0; count < 4096; count++)
   {
      char line[96], operation, trailing;
      size_t path_length, length;
      int next = read_line(STDIN_FILENO, line, sizeof line);
      if (!next)
         break;
      if (next < 0 ||
          sscanf(line, "raw-file-v1 %c %zu %zu %c", &operation, &path_length, &length, &trailing) !=
              3 ||
          (operation != 'R' && operation != 'W' && operation != 'P' && operation != 'D') ||
          !path_length || path_length > 4095 || length > SCOPED_FILE_IO_MAX ||
          ((operation == 'R' || operation == 'D') && length))
      {
         rc = 1;
         break;
      }
      char *path = malloc(path_length + 1), *input = length ? malloc(length) : NULL;
      if (!path || (length && !input) || read_exact(STDIN_FILENO, path, path_length) ||
          (length && read_exact(STDIN_FILENO, input, length)))
      {
         free(path);
         free(input);
         rc = 1;
         break;
      }
      path[path_length] = 0;
      scoped_file_io_result_t result = {0};
      int failed = strlen(path) != path_length ||
                   (operation == 'D'
                        ? scoped_directory_io(root, root_path, path, &result)
                        : scoped_file_io_parents(root, root_path, path, operation != 'R', input,
                                                 length, NULL, NULL, &result, operation == 'P'));
      int status = failed ? (result.effect_started ? 2 : 1) : 0;
      size_t output = !status && (operation == 'R' || operation == 'D') ? result.length : 0;
      int n = snprintf(line, sizeof line, "raw-file-v1 %d %zu\n", status, output);
      rc = n < 0 || (size_t)n >= sizeof line || write_all(STDOUT_FILENO, line, (size_t)n) ||
           (output && write_all(STDOUT_FILENO, result.bytes, output));
      free(result.bytes);
      free(path);
      free(input);
      if (rc)
         break;
   }
   close(root);
   (void)rmdir(home); /* Container retirement owns any nonempty scratch home. */
   return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
   int owned_forwarder = argc > 1 && !strcmp(argv[1], "--owned-forwarder");
   if (owned_forwarder && (argc != 4 || (strcmp(argv[2], "3131") && strcmp(argv[2], "3129")) ||
                           strcmp(argv[3], DEFAULT_SOCK) || owner_nondumpable()))
      return 1;
   if (argc > 1 && !owned_forwarder)
   {
      signal(SIGPIPE, SIG_IGN);
      if (argc == 2 && !strcmp(argv[1], "--idle-guard"))
      {
         if (owner_nondumpable())
            return 1;
         signal(SIGCHLD, reap_children);
         for (;;)
            pause();
      }
      return argc == 4 && !strcmp(argv[1], "--file-serve") ? file_serve(argv[2], argv[3]) : 1;
   }
   const char *sock = owned_forwarder ? argv[3] : getenv("AIMEE_FORWARDER_SOCK");
   if (!sock || !sock[0])
      sock = DEFAULT_SOCK;
   int port = DEFAULT_PORT;
   const char *ps = owned_forwarder ? argv[2] : getenv("AIMEE_FORWARDER_PORT");
   if (ps && ps[0])
   {
      int p = atoi(ps);
      if (p > 0 && p < 65536)
         port = p;
   }

   signal(SIGCHLD, reap_children); /* reap + decrement the concurrency counter */
   signal(SIGPIPE, SIG_IGN);

   int lfd = socket(AF_INET, SOCK_STREAM, 0);
   if (lfd < 0)
   {
      perror("socket");
      return 1;
   }
   int one = 1;
   setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in a;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only */
   a.sin_port = htons((uint16_t)port);
   if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 64) != 0)
   {
      perror("bind/listen");
      return 1;
   }
   if (owned_forwarder &&
       write_all(STDOUT_FILENO, "raw-forwarder-v1 ready\n", sizeof "raw-forwarder-v1 ready\n" - 1))
   {
      close(lfd);
      return 1;
   }

   for (;;)
   {
      int c = accept(lfd, NULL, NULL);
      if (c < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      /* Concurrency cap: refuse past MAX_CHILDREN so a delegate cannot exhaust PIDs/
       * FDs/UDS slots by opening thousands of tunnels. */
      if (g_children >= MAX_CHILDREN)
      {
         close(c);
         continue;
      }
      /* Block SIGCHLD across fork + increment so the reaper cannot decrement for a
       * child that exits before the parent has counted it (which would leak the
       * counter up to MAX_CHILDREN and then permanently refuse). */
      sigset_t chld, prev;
      sigemptyset(&chld);
      sigaddset(&chld, SIGCHLD);
      sigprocmask(SIG_BLOCK, &chld, &prev);
      pid_t pid = fork();
      if (pid == 0)
      {
         sigprocmask(SIG_SETMASK, &prev, NULL); /* child: restore the mask */
         close(lfd);
         int u = connect_uds(sock);
         if (u >= 0)
         {
            pump(c, u);
            close(u);
         }
         close(c);
         _exit(0);
      }
      if (pid > 0)
         g_children++;
      sigprocmask(SIG_SETMASK, &prev, NULL); /* parent: unblock — reaper sees the increment */
      close(c);
   }
   close(lfd);
   return 0;
}
