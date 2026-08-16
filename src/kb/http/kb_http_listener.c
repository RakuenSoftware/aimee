/* kb_http_listener.c: bounded concurrent plain-HTTP listener for aimee-kb. */

#include "kb_http.h"
#include "modules/db2/c/db2.h"
#include "kb/kb_login_throttle.h"
#include "log.h"
#include <sys/stat.h>
#include "kb_paths.h"

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

/* Idle timeout for an accepted connection, applied per read/write rather than to
 * the request as a whole: a large ingest that keeps producing data resets it on
 * every chunk, while a peer that goes silent is dropped and its worker returned.
 * Without this a client that connects and never sends holds a worker forever --
 * sixteen such sockets took the entire kb offline (see the listener loop). */
#define KB_HTTP_IO_TIMEOUT_S 60

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
      /* Claim the worker slot BEFORE accepting, not after.
       *
       * Accepting first and then blocking on the slot means a connection that
       * has already been taken out of the kernel's accept queue sits in user
       * space unread and unanswered for as long as the workers stay busy, and
       * the loop does not return to accept() while it waits. If the workers
       * never free -- one wedged handler per slot is enough -- the listener is
       * dead: the backlog fills, nothing new is accepted, and every fd already
       * pulled out of it is stranded.
       *
       * Observed on a live kb whose 16 workers were all blocked. The listen
       * socket had a full accept queue (Recv-Q 17, backlog 16) and the accepted
       * sockets sat in CLOSE-WAIT holding unread request bytes -- peers had
       * given up and closed, and nothing ever read or closed this side. Health
       * checks could not even connect, so the container was marked unhealthy
       * with no error logged anywhere.
       *
       * Waiting for the slot first leaves pending connections queued in the
       * kernel where they belong: they are still there when a worker frees, and
       * once the backlog is full new peers get a prompt refusal instead of an
       * accepted socket nobody will ever serve. It does not stop handlers from
       * wedging -- it stops one wedged handler from taking the listener and a
       * pile of file descriptors down with it. */
      pthread_mutex_lock(&g_worker_mutex);
      while (g_running && g_worker_count >= KB_HTTP_WORKERS)
         pthread_cond_wait(&g_worker_cond, &g_worker_mutex);
      if (!g_running)
      {
         pthread_mutex_unlock(&g_worker_mutex);
         break;
      }
      g_worker_count++;
      pthread_mutex_unlock(&g_worker_mutex);

      struct sockaddr_in addr;
      socklen_t addrlen = sizeof(addr);
      int fd = accept(g_listen_fd, (struct sockaddr *)&addr, &addrlen);
      if (fd < 0)
      {
         /* The slot was claimed above; give it back or the pool bleeds one
          * worker per failed accept until the listener can never run again. */
         worker_finished();
         if (g_running)
            LOG_WARN("kb_http", "accept failed: %s", strerror(errno));
         break;
      }
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      /* A silent peer must not own a worker indefinitely. read() then returns
       * -1/EAGAIN, which both read loops in kb_http_conn.c already treat as
       * end-of-input, so the connection is closed and the slot handed back. */
      struct timeval io_timeout = {.tv_sec = KB_HTTP_IO_TIMEOUT_S, .tv_usec = 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
      char peer[INET_ADDRSTRLEN] = "";
      if (!inet_ntop(AF_INET, &addr.sin_addr, peer, sizeof(peer)))
         peer[0] = '\0';

      if (spawn_connection_worker(fd, peer) != 0)
      {
         serve_connection(fd, peer);
         worker_finished();
      }
   }
   return NULL;
}

/* Marker recording that this deployment has had a kb bearer. Written the first
 * time one is present; its existence turns a later missing bearer from a
 * warning into a refusal. Best-effort: if it cannot be written we degrade to
 * warning rather than refusing to serve. */
static void bearer_marker_path(char *out, size_t n)
{
   snprintf(out, n, "%s/kb-bearer-sealed", kb_default_config_dir());
}

/* Warn while this deployment has never been sealed; refuse once it has.
 * Returns 0 to continue binding, -1 to refuse. */
static int enforce_bearer_ratchet(int port)
{
   char marker[MAX_PATH_LEN];
   bearer_marker_path(marker, sizeof(marker));

   if (g_bearer_token[0])
   {
      /* Sealed. Record it so a later boot without a bearer is a hard failure. */
      struct stat st;
      if (stat(marker, &st) != 0)
      {
         int fd = open(marker, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
         if (fd >= 0)
         {
            close(fd);
            LOG_INFO("kb_http",
                     "kb bearer present; recorded %s — a later boot without a bearer "
                     "will now refuse to bind a non-loopback plain listener",
                     marker);
         }
      }
      return 0;
   }

   struct stat st;
   if (stat(marker, &st) == 0)
   {
      LOG_ERROR("kb_http",
                "refusing to bind 0.0.0.0:%d: this deployment was sealed with a kb bearer (%s "
                "exists) but none is configured now, so every privileged route would answer "
                "unauthenticated. Re-seal AIMEE_KB_API_BEARER_TOKEN through the first-boot Vault "
                "bootstrap, or remove that marker to deliberately return to an unauthenticated "
                "listener.",
                port, marker);
      return -1;
   }

   LOG_ERROR("kb_http",
             "binding 0.0.0.0:%d with NO bearer configured: this socket serves privileged routes "
             "unauthenticated to anything that can reach it. Seal a kb bearer "
             "(AIMEE_KB_API_BEARER_TOKEN, through the first-boot Vault bootstrap), or unset "
             "AIMEE_KB_HTTP_BIND to keep the plain listener on loopback.",
             port);
   return 0;
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

   /* Auth is only enforced when a bearer is configured (kb_http.c's gate is
    * `if (bearer_token && bearer_token[0])`). With none, the kb answers every
    * route unauthenticated — which is a reasonable default for the loopback-only
    * bind above, and is NOT reasonable once this socket is reachable off-host.
    *
    * Observed on a real deployment: `POST /v1/actions/memory.find_facts` with no
    * credentials at all returned 200 with stored memory, to anything that could
    * route to the container. Fail closed instead: a non-loopback bind requires a
    * bearer. The mTLS listener is unaffected — it authenticates by certificate. */
   /* Auth is enforced only when a bearer is configured (kb_http.c gates on a
    * non-empty bearer). With none, this socket answers every route
    * unauthenticated — fine for the loopback default, not fine once it is
    * reachable off-host.
    *
    * Ratchet rather than a flat refusal. A flat refusal would break every
    * deployment that has not been sealed yet, which today is all of them; a flat
    * warning would let a sealed deployment silently lose its bearer and reopen
    * the hole. So: warn while a deployment has never been sealed, and refuse
    * once it has. The marker is written the first time a bearer is present, and
    * lives beside the rest of the kb's state so it persists exactly as long as
    * the vault it corresponds to. */
   if (baddr == INADDR_ANY && enforce_bearer_ratchet(port) != 0)
   {
      close(g_listen_fd);
      g_listen_fd = -1;
      return -1;
   }

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
