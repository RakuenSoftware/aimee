/* server_conn_io.c: see server_conn_io.h. fd->SSL registry + transport-aware I/O. */
/* _GNU_SOURCE: accept4() is a GNU extension; declare it before any include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_conn_io.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

/* fds are small ints; a flat array keyed by fd is O(1) and avoids a hash. Sized
 * well above any realistic server fd count. A fd outside the range falls back to
 * the raw path (never crashes). */
#define CONN_IO_MAX_FD 65536

static SSL *g_fd_ssl[CONN_IO_MAX_FD];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void server_conn_io_set_ssl(int fd, SSL *ssl)
{
   if (fd < 0 || fd >= CONN_IO_MAX_FD)
      return;
   pthread_mutex_lock(&g_mu);
   g_fd_ssl[fd] = ssl;
   pthread_mutex_unlock(&g_mu);
}

void server_conn_io_clear(int fd)
{
   server_conn_io_set_ssl(fd, NULL);
}

/* A pointer-sized load is atomic on supported platforms; the GET path stays
 * lock-free so the hot I/O loop is not serialized. set/clear hold the mutex. */
static SSL *ssl_for(int fd)
{
   if (fd < 0 || fd >= CONN_IO_MAX_FD)
      return NULL;
   return g_fd_ssl[fd];
}

int server_conn_io_has_ssl(int fd)
{
   return ssl_for(fd) != NULL;
}

SSL *server_conn_io_get_ssl(int fd)
{
   return ssl_for(fd);
}

int server_conn_io_read(int fd, void *buf, int n)
{
   SSL *s = ssl_for(fd);
   if (s)
      return SSL_read(s, buf, n);
   return (int)read(fd, buf, (size_t)n);
}

int server_conn_io_write_all(int fd, const void *buf, int n)
{
   SSL *s = ssl_for(fd);
   const char *p = (const char *)buf;
   int off = 0;
   while (off < n)
   {
      int w = s ? SSL_write(s, p + off, n - off) : (int)write(fd, p + off, (size_t)(n - off));
      if (w <= 0)
         return -1;
      off += w;
   }
   return off;
}

/* Accept a connection with close-on-exec set, so no child process this server
 * spawns can inherit a client's socket.
 *
 * An inherited connection fd does not merely leak: it HANGS THE CLIENT. The
 * client (http_uds_client.c) reads the response until EOF and ignores
 * Content-Length, and EOF arrives only when every copy of the socket is closed.
 * This server forks constantly — git, delegates, deploys, process_mgr — and any
 * child forked while a request is in flight inherits that request's fd and pins
 * it open for the child's whole lifetime. Observed in production as a
 * `workspace add` that never returned: the server had answered and closed its
 * copy, but a background `git cat-file --batch` forked during the request still
 * held the fd, so the client sat in read() for 28 minutes. Small/fast requests
 * hide it — the window has to overlap a fork.
 *
 * accept4() sets the flag ATOMICALLY, which matters here: a post-accept fcntl()
 * leaves a window in which another thread can fork and inherit the fd anyway.
 * macOS/BSD have no accept4, so fall back to accept()+fcntl() there and accept
 * the narrow race the platform forces. Mirrors cli_fd_cloexec() in
 * posix/cli_client.c. */
int server_conn_accept(int listen_fd)
{
   int fd = -1;
#ifdef SOCK_CLOEXEC
   fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
   if (fd >= 0 || errno != ENOSYS)
      return fd;
   /* ENOSYS: kernel older than the libc that offered accept4 — fall through. */
#endif
   fd = accept(listen_fd, NULL, NULL);
   if (fd >= 0)
   {
      int flags = fcntl(fd, F_GETFD);
      if (flags >= 0)
         (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
   }
   return fd;
}
