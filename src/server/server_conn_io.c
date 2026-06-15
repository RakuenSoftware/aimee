/* server_conn_io.c: see server_conn_io.h. fd->SSL registry + transport-aware I/O. */
#include "server_conn_io.h"

#include <pthread.h>
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
