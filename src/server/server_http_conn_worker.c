/* server_http_conn_worker.c: split from server_http.c into a real translation unit
 * (was server_http_conn_worker.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_http_internal.h"
#include "server_http.h"
#include "server.h"         /* CAP_* / CAPS_* capability bits, server_capability_for_method */
#include "server_conn_io.h" /* transport-aware fd I/O (native-TLS phase 1) */
#include "server_tls.h"     /* native TLS termination (phase 1b) */
#include "modules/workspace/workspace_runner_registry.h" /* ws_runner_registry_poll/_respond for the /v1 reverse channel */
#include <time.h>
#include "persona.h"
#include "role_templates.h"
#include "util.h" /* safe_strdup, aimee_base64_* */
#include "cli_session_pty.h"
#include "config.h"
#include "prompts.h"
#include <aimee/delegates/delegate_role.h>
#include "log.h"
#include "aimee_version.h"
#include "openai_shape.h"
#include "ingress_preinject.h"
#include "openapi_server_data.h" /* AIMEE_OPENAPI_SERVER_YAML_STR (generated from api/openapi-server-v1.yaml) */
#include "openai_runs_store.h"
#if AIMEE_WITH_ROUNDTABLE
#include "roundtable_pipeline_capture.h" /* pipeline op-run capture seam (#18/#20) */
#endif
#include "presence.h"
#include "request_context.h"
#include "server_http_identity.h" /* WP-C.0 attested-identity capture/threading */
#include "server_workflow_api.h"  /* W7: /v1/workflow read+author handlers */
#include "cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

static void *conn_worker(void *arg)
{
   conn_job_t *j = (conn_job_t *)arg;
   /* TLS handshake runs HERE (in the worker, never blocking the accept loop) and
    * registers the SSL on the conn-io shim; this worker owns it end to end
    * (SSE-offload is refused over TLS, so it never crosses threads). */
   SSL *ssl = j->is_management ? server_tls_management_begin(j->fd)
                               : (j->is_tls ? server_tls_begin(j->fd) : NULL);
   if (!(j->is_tls && !ssl)) /* skip handle_conn only when the TLS handshake failed */
   {
      do
      {
         handle_conn(j->fd, j->is_tcp, j->is_management);
      } while (j->is_tls && server_http_keepalive_take());
   }
   server_tls_end(j->fd, ssl);
   close(j->fd);
   atomic_fetch_sub(j->is_management ? &g_management_conn_live : &g_conn_live, 1);
   free(j);
   return NULL;
}

/* Hand an accepted connection to a detached worker (which closes fd). Returns 1
 * if offloaded, 0 if the caller should handle it inline (cap hit / no resources). */
int conn_offload(int fd, int is_tcp, int is_tls, int is_management)
{
   atomic_int *live = is_management ? &g_management_conn_live : &g_conn_live;
   int live_max = is_management ? CONN_MANAGEMENT_LIVE_MAX : CONN_LIVE_MAX;
   if (atomic_fetch_add(live, 1) >= live_max)
   {
      atomic_fetch_sub(live, 1);
      return 0;
   }
   conn_job_t *j = (conn_job_t *)malloc(sizeof(*j));
   if (!j)
   {
      atomic_fetch_sub(live, 1);
      return 0;
   }
   j->fd = fd;
   j->is_tcp = is_tcp;
   j->is_tls = is_tls;
   j->is_management = is_management;
   pthread_attr_t attr;
   pthread_attr_t *ap = NULL;
   if (pthread_attr_init(&attr) == 0)
   {
      if (pthread_attr_setstacksize(&attr, (size_t)32 * 1024 * 1024) == 0)
         ap = &attr;
   }
   pthread_t t;
   int rc = pthread_create(&t, ap, conn_worker, j);
   if (ap)
      pthread_attr_destroy(&attr);
   if (rc != 0)
   {
      free(j);
      atomic_fetch_sub(live, 1);
      return 0;
   }
   pthread_detach(t);
   return 1;
}
