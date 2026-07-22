#include "server_kb_heartbeat.h"
#include "kb_client_mtls.h"
#include "aimee.h"
#include "log.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_t g_thread;
static atomic_int g_running;
static int g_started;
static char g_server_id[128];

static void *heartbeat_main(void *unused)
{
   (void)unused;
   while (atomic_load(&g_running))
   {
      if (kb_client_mtls_heartbeat(g_server_id, "ready", AIMEE_VERSION) != 0)
         LOG_WARN("server.kb", "server registry heartbeat rejected for %s", g_server_id);
      for (int i = 0; i < 30 && atomic_load(&g_running); i++)
      {
         struct timespec delay = {1, 0};
         nanosleep(&delay, NULL);
      }
   }
   return NULL;
}

int server_kb_heartbeat_start(const char *server_id)
{
   if (!kb_client_mtls_configured())
      return 0;
   size_t n = server_id ? strlen(server_id) : 0;
   if (!n || n >= sizeof(g_server_id))
      return -1;
   for (size_t i = 0; i < n; i++)
      if (!(isalnum((unsigned char)server_id[i]) || strchr("._-", server_id[i])))
         return -1;
   snprintf(g_server_id, sizeof(g_server_id), "%s", server_id);
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
