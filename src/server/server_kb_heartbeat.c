#include "server_kb_heartbeat.h"
#include "kb_client_mtls.h"
#include "server_mgmt_jwks_cache.h"
#include "server_runtime_identity.h"
#include "aimee.h"
#include "log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static pthread_t g_thread;
static atomic_int g_running;
static int g_started;

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
      /* Before the wizard's one-shot writes the identity, poll quickly. Once
       * enrolled, return to the ordinary 30-second registry cadence. */
      int interval = ready ? 30 : 1;
      for (int i = 0; i < interval && atomic_load(&g_running); i++)
      {
         struct timespec delay = {1, 0};
         nanosleep(&delay, NULL);
      }
   }
   return NULL;
}

int server_kb_heartbeat_start(void)
{
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
   pthread_join(g_thread, NULL);
   g_started = 0;
}
