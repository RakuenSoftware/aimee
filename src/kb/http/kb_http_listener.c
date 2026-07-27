/* kb_http_listener.c: bounded concurrent plain-HTTP listener for aimee-kb. */

#include "kb_http.h"
#include "db2/db2.h"
#include "kb/kb_login_throttle.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define KB_HTTP_BACKLOG 64
#define KB_HTTP_WORKERS 16

void handle_connection(int fd);

typedef struct
{
   int fd;
   char peer[INET_ADDRSTRLEN];
} connection_arg_t;

static pthread_t g_thread;
static int g_listen_fd = -1;
static volatile int g_running = 0;
static pthread_mutex_t g_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_worker_cond = PTHREAD_COND_INITIALIZER;
static int g_worker_count = 0;
char g_bearer_token[256];

static void serve_connection(int fd, const char *peer)
{
   kb_login_throttle_set_peer(peer);
   handle_connection(fd);
   kb_login_throttle_set_peer("");
   /* db2_conn() leases lazily per thread. A worker is one request, so return
    * its lease instead of relying on implementation-defined TLS teardown. */
   db2_lease_release_idle();
   close(fd);
}

static void *connection_worker(void *arg)
{
   connection_arg_t connection = *(connection_arg_t *)arg;
   free(arg);
   serve_connection(connection.fd, connection.peer);

   pthread_mutex_lock(&g_worker_mutex);
   g_worker_count--;
   pthread_cond_broadcast(&g_worker_cond);
   pthread_mutex_unlock(&g_worker_mutex);
   return NULL;
}

static int spawn_connection_worker(int fd, const char *peer)
{
   connection_arg_t *arg = malloc(sizeof(*arg));
   if (!arg)
      return -1;
   arg->fd = fd;
   snprintf(arg->peer, sizeof(arg->peer), "%s", peer ? peer : "");

   pthread_attr_t attr;
   pthread_attr_t *attrp = NULL;
   int attr_init = pthread_attr_init(&attr) == 0;
   if (attr_init)
   {
      /* Memory search/store has large nested config and result frames. Live
       * concurrency overflowed 4 MiB; 16 MiB retains explicit headroom. */
      if (pthread_attr_setstacksize(&attr, (size_t)16 * 1024 * 1024) == 0)
         attrp = &attr;
   }

   pthread_t tid;
   int rc = pthread_create(&tid, attrp, connection_worker, arg);
   if (attr_init)
      pthread_attr_destroy(&attr);
   if (rc != 0)
   {
      free(arg);
      return -1;
   }
   pthread_detach(tid);
   return 0;
}

static void worker_finished(void)
{
   pthread_mutex_lock(&g_worker_mutex);
   g_worker_count--;
   pthread_cond_broadcast(&g_worker_cond);
   pthread_mutex_unlock(&g_worker_mutex);
}

static void *listener_thread(void *arg)
{
   (void)arg;
   while (g_running)
   {
      struct sockaddr_in addr;
      socklen_t addrlen = sizeof(addr);
      int fd = accept(g_listen_fd, (struct sockaddr *)&addr, &addrlen);
      if (fd < 0)
      {
         if (g_running)
            LOG_WARN("kb_http", "accept failed: %s", strerror(errno));
         break;
      }
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      char peer[INET_ADDRSTRLEN] = "";
      if (!inet_ntop(AF_INET, &addr.sin_addr, peer, sizeof(peer)))
         peer[0] = '\0';

      pthread_mutex_lock(&g_worker_mutex);
      while (g_running && g_worker_count >= KB_HTTP_WORKERS)
         pthread_cond_wait(&g_worker_cond, &g_worker_mutex);
      if (!g_running)
      {
         pthread_mutex_unlock(&g_worker_mutex);
         close(fd);
         break;
      }
      g_worker_count++;
      pthread_mutex_unlock(&g_worker_mutex);

      if (spawn_connection_worker(fd, peer) != 0)
      {
         serve_connection(fd, peer);
         worker_finished();
      }
   }
   return NULL;
}

int kb_http_start(int port, const char *bearer_token)
{
   if (port <= 0)
      return 0;

   g_bearer_token[0] = '\0';
   if (bearer_token && bearer_token[0])
      snprintf(g_bearer_token, sizeof(g_bearer_token), "%s", bearer_token);

   g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   if (g_listen_fd < 0)
      return -1;
   fcntl(g_listen_fd, F_SETFD, FD_CLOEXEC);
   int opt = 1;
   setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   const char *b = getenv("AIMEE_KB_HTTP_BIND");
   in_addr_t baddr = (b && b[0]) ? INADDR_ANY : INADDR_LOOPBACK;
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl(baddr);
   sa.sin_port = htons((uint16_t)port);

   if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
       listen(g_listen_fd, KB_HTTP_BACKLOG) < 0)
   {
      close(g_listen_fd);
      g_listen_fd = -1;
      return -1;
   }

   g_running = 1;
   if (pthread_create(&g_thread, NULL, listener_thread, NULL) != 0)
   {
      g_running = 0;
      close(g_listen_fd);
      g_listen_fd = -1;
      return -1;
   }

   LOG_INFO("kb_http", "listening on %s:%d", baddr == INADDR_ANY ? "0.0.0.0" : "127.0.0.1", port);
   return 0;
}

void kb_http_stop(void)
{
   if (!g_running)
      return;
   g_running = 0;
   if (g_listen_fd >= 0)
   {
      shutdown(g_listen_fd, SHUT_RDWR);
      close(g_listen_fd);
      g_listen_fd = -1;
   }
   pthread_mutex_lock(&g_worker_mutex);
   pthread_cond_broadcast(&g_worker_cond);
   pthread_mutex_unlock(&g_worker_mutex);
   pthread_join(g_thread, NULL);
   pthread_mutex_lock(&g_worker_mutex);
   while (g_worker_count > 0)
      pthread_cond_wait(&g_worker_cond, &g_worker_mutex);
   pthread_mutex_unlock(&g_worker_mutex);
}
