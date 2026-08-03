/* test_server_conn_accept.c: an accepted connection fd must never be inherited
 * by a child process.
 *
 * This is not a descriptor-hygiene nicety. The CLI reads its response until EOF
 * (http_uds_client.c ignores Content-Length), and EOF arrives only once EVERY
 * copy of the socket is closed. So a child that inherits a request's fd keeps
 * that client blocked in read() for as long as the child lives — which in
 * production was a background `git cat-file --batch` and a 28-minute hang on a
 * `workspace add` the server had already answered.
 *
 * The test therefore asserts the OBSERVABLE failure, not the flag: server
 * answers, server closes, a long-lived child is still running — does the client
 * see EOF? With plain accept() it does not. */
#include "server_conn_io.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *sock_path(void)
{
   static char path[256];
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = "/tmp";
   snprintf(path, sizeof(path), "%s/aimee-cloexec-%d.sock", tmp, (int)getpid());
   return path;
}

/* Serve one connection, then hold a forked child alive with the fd table as it
 * was at accept time. |use_cloexec| picks the accept path under test.
 * Returns the child's pid; *out_fd receives the client end. */
static pid_t serve_once(const char *path, int use_cloexec, int *out_fd)
{
   int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(lfd >= 0);
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
   unlink(path);
   assert(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(lfd, 4) == 0);

   int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(cfd >= 0);
   assert(connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

   int afd;
   if (use_cloexec)
      afd = server_conn_accept(lfd);
   else
      afd = accept(lfd, NULL, NULL); /* the stock path, for the red case */
   assert(afd >= 0);

   /* Fork+exec a long-lived child WHILE the connection is open. exec is what
    * makes close-on-exec meaningful: a bare fork() inherits either way. */
   pid_t pid = fork();
   assert(pid >= 0);
   if (pid == 0)
   {
      execl("/bin/sh", "sh", "-c", "sleep 30", (char *)NULL);
      _exit(127);
   }

   /* Server answers and closes its copy — the client should now see EOF. */
   const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nok";
   ssize_t w = write(afd, resp, strlen(resp));
   (void)w;
   close(afd);
   close(lfd);
   unlink(path);
   *out_fd = cfd;
   return pid;
}

/* Drain the client fd until EOF or timeout. 1 = saw EOF, 0 = still open. */
static int client_sees_eof(int fd, int timeout_ms)
{
   struct timeval tv;
   tv.tv_sec = timeout_ms / 1000;
   tv.tv_usec = (timeout_ms % 1000) * 1000;
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   char buf[512];
   for (;;)
   {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n == 0)
         return 1; /* EOF: every copy of the socket is closed */
      if (n < 0)
         return 0; /* timed out with the socket still open somewhere */
   }
}

/* Red case: the stock accept() path leaks the fd, so the client never sees EOF
 * while the child lives. Asserting this keeps the test honest — if it ever
 * stops reproducing, the test below is no longer proving anything. */
static void test_plain_accept_hangs_the_client(void)
{
   int cfd = -1;
   pid_t child = serve_once(sock_path(), 0, &cfd);
   int eof = client_sees_eof(cfd, 1500);
   close(cfd);
   kill(child, SIGKILL);
   waitpid(child, NULL, 0);
   assert(eof == 0); /* the bug: response delivered, but no EOF — client blocks */
   printf("  test_plain_accept_hangs_the_client: ok (reproduced)\n");
}

static void test_server_conn_accept_releases_the_client(void)
{
   int cfd = -1;
   pid_t child = serve_once(sock_path(), 1, &cfd);
   int eof = client_sees_eof(cfd, 5000);
   close(cfd);
   kill(child, SIGKILL);
   waitpid(child, NULL, 0);
   assert(eof == 1); /* child cannot hold the fd: client completes immediately */
   printf("  test_server_conn_accept_releases_the_client: ok\n");
}

int main(void)
{
   printf("test_server_conn_accept:\n");
   signal(SIGPIPE, SIG_IGN);
   test_plain_accept_hangs_the_client();
   test_server_conn_accept_releases_the_client();
   printf("ALL PASS\n");
   return 0;
}
