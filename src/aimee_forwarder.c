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

int main(void)
{
   const char *sock = getenv("AIMEE_FORWARDER_SOCK");
   if (!sock || !sock[0])
      sock = DEFAULT_SOCK;
   int port = DEFAULT_PORT;
   const char *ps = getenv("AIMEE_FORWARDER_PORT");
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
      pid_t pid = fork();
      if (pid == 0)
      {
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
      close(c);
   }
   close(lfd);
   return 0;
}
