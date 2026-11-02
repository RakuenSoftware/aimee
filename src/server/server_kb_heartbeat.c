#include "server_kb_heartbeat.h"
#include "kb_client_mtls.h"
#include "server_mgmt_jwks_cache.h"
#include "server_runtime_identity.h"
#include "aimee.h"
#include "config.h"
#include "log.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

static pthread_t g_thread;
static atomic_int g_running;
static int g_started;
static int g_wake_fd = -1;

/* The registry heartbeat is a lease the KB expires; it keeps its cadence. */
#define HEARTBEAT_INTERVAL_MS 30000
/* Until enrolled, the files that enrol this server are watched, and config
 * changes are announced. This bounds a change that reached neither, such as a
 * connection secret set outside the config directory. */
#define ENROLL_RECHECK_MS 300000

/* The wizard creates the managed KB (and therefore its hostname-valid server
 * certificate) after aimee-server is already running. The startup JWKS fetch is
 * intentionally fail-closed, but a one-shot failure must not require an
 * operator restart: the first authenticated heartbeat proves the same mTLS
 * identity can now reach the KB, so retry the signed-artifact fetch at that
 * point. Token verification continues to own later generation refreshes. */
static int management_jwks_ready(void)
{
   const char *trust_path = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   if (!trust_path || !trust_path[0])
      return 1; /* Management authorization is not configured for this server. */
   return server_mgmt_jwks_cache_startup(trust_path, (int64_t)time(NULL),
                                         kb_client_mtls_management_jwks_fetch,
                                         NULL) == SERVER_MGMT_JWKS_CACHE_OK;
}

/* Watch the directories the client identity and the listener identity are
 * written into. -1 when inotify is unavailable; the wait then only times out. */
#define ENROLL_EVENTS (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE)

/* tls/ may not exist yet; the base watch sees it made, and this adds it then.
 * Adding a watch that exists is a no-op. */
static void watch_tls(int fd)
{
   const char *base = config_default_dir();
   char tls[1024];
   if (fd >= 0 && base && snprintf(tls, sizeof tls, "%s/tls", base) < (int)sizeof tls)
      (void)inotify_add_watch(fd, tls, ENROLL_EVENTS);
}

static int enrollment_watch(void)
{
   const char *base = config_default_dir();
   if (!base || !base[0])
      return -1;
   int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
   if (fd < 0)
      return -1;
   if (inotify_add_watch(fd, base, ENROLL_EVENTS) < 0)
   {
      close(fd);
      return -1;
   }
   watch_tls(fd);
   return fd;
}

static void drain(int fd)
{
   char buffer[4096];
   while (fd >= 0 && read(fd, buffer, sizeof buffer) > 0)
   {
   }
}

static void heartbeat_wait(int ready)
{
   static int watch = -2;
   if (!ready && watch == -2)
      watch = enrollment_watch();
   struct pollfd fds[2] = {{.fd = g_wake_fd, .events = POLLIN},
                           {.fd = ready ? -1 : watch, .events = POLLIN}};
   int timeout = ready ? HEARTBEAT_INTERVAL_MS : ENROLL_RECHECK_MS;
   if (atomic_load(&g_running) && poll(fds, 2, timeout) < 0 && errno != EINTR)
   {
      struct timespec delay = {1, 0}; /* a broken poll must not spin */
      nanosleep(&delay, NULL);
   }
   drain(g_wake_fd);
   if (!ready)
   {
      drain(watch);
      watch_tls(watch);
   }
}

void server_kb_heartbeat_wake(void)
{
   uint64_t one = 1;
   if (g_wake_fd >= 0)
      (void)!write(g_wake_fd, &one, sizeof one);
}

static void *heartbeat_main(void *unused)
{
   (void)unused;
   int jwks_ready = 0;
   while (atomic_load(&g_running))
   {
      char server_id[128];
      int ready = server_runtime_server_id_load(server_id, sizeof(server_id)) &&
                  kb_client_mtls_configured();
      if (ready)
      {
         if (kb_client_mtls_heartbeat(server_id, "ready", AIMEE_VERSION) != 0)
            LOG_WARN("server.kb", "server registry heartbeat rejected for %s", server_id);
         else if (!jwks_ready && management_jwks_ready())
         {
            jwks_ready = 1;
            LOG_INFO("server.mgmt", "management JWKS authorization ready");
         }
      }
      /* Before the wizard's one-shot writes the identity, sleep until a file
       * in the config directory changes (the identity is written there) or a
       * config change is announced. Once enrolled, keep the 30-second
       * registry cadence. */
      heartbeat_wait(ready);
   }
   return NULL;
}

int server_kb_heartbeat_start(void)
{
   if (g_wake_fd < 0)
      g_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   atomic_store(&g_running, 1);
   if (pthread_create(&g_thread, NULL, heartbeat_main, NULL) != 0)
   {
      atomic_store(&g_running, 0);
      return -1;
   }
   g_started = 1;
   return 0;
}

void server_kb_heartbeat_stop(void)
{
   if (!g_started)
      return;
   atomic_store(&g_running, 0);
   server_kb_heartbeat_wake();
   pthread_join(g_thread, NULL);
   g_started = 0;
}
