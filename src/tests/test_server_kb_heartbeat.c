/* The registry heartbeat worker of a server that is not enrolled sleeps until a
 * file in the config directory changes or config is announced, instead of
 * re-reading its identity every second; it stops promptly. */
#include "server_kb_heartbeat.h"
#include "kb_client_mtls.h"
#include "server/server_mgmt_jwks_cache.h"
#include "server/server_runtime_identity.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_dir[256];
static atomic_int loads;

const char *config_default_dir(void)
{
   return g_dir;
}
int server_runtime_server_id_load(char *server_id, size_t cap)
{
   atomic_fetch_add(&loads, 1);
   if (server_id && cap)
      server_id[0] = '\0';
   return 0; /* never enrolled */
}
int kb_client_mtls_configured(void)
{
   return 0;
}
int kb_client_mtls_heartbeat(const char *server_id, const char *health, const char *version)
{
   (void)server_id, (void)health, (void)version;
   assert(!"an unenrolled server sends no heartbeat");
   return -1;
}
int kb_client_mtls_management_jwks_fetch(void *ctx, char *out, size_t cap, size_t *len)
{
   (void)ctx, (void)out, (void)cap, (void)len;
   return -1;
}
server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_startup(const char *path, int64_t now,
                                                               server_mgmt_jwks_fetch_fn fetch,
                                                               void *ctx)
{
   (void)path, (void)now, (void)fetch, (void)ctx;
   return SERVER_MGMT_JWKS_CACHE_OK;
}

static void settle(int tenths)
{
   for (int n = 0; n < tenths; n++)
   {
      struct timespec pause = {.tv_nsec = 100000000L};
      nanosleep(&pause, NULL);
   }
}

static int loads_after(int before)
{
   for (int n = 0; n < 30 && atomic_load(&loads) == before; n++)
      settle(1);
   return atomic_load(&loads);
}

int main(void)
{
   const char *tmp = getenv("TMPDIR");
   snprintf(g_dir, sizeof g_dir, "%s/kb-heartbeat-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
   assert(mkdtemp(g_dir));

   assert(server_kb_heartbeat_start() == 0);
   int first = loads_after(0);
   assert(first == 1);
   settle(25); /* the old loop would have re-read twice by now */
   assert(atomic_load(&loads) == first);

   char path[512];
   snprintf(path, sizeof path, "%s/kb-client-identity.json", g_dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("{}", f);
   fclose(f);
   int written = loads_after(first);
   assert(written > first); /* the identity being written is seen at once */

   settle(3);
   int quiet = atomic_load(&loads);
   server_kb_heartbeat_wake();
   assert(loads_after(quiet) > quiet); /* an announced config change too */

   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);
   server_kb_heartbeat_stop();
   clock_gettime(CLOCK_MONOTONIC, &end);
   assert(end.tv_sec - start.tv_sec < 2);

   unlink(path);
   rmdir(g_dir);
   puts("server kb heartbeat: an unenrolled server waits for its identity, not a timer");
   return 0;
}
