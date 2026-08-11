/* server_http_sse.c: split from server_http.c into a real translation unit
 * (was server_http_sse.inc, textually included only to stay under the
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

void handle_session_events(int fd, const char *id_in, const char *request_id)
{
   /* id_in may carry a resume position as "<sid>?cursor=N" (threaded from the
    * dispatcher's query string). Split it: `id` is the bare session id, `cursor`
    * the absolute ring position to resume from (0 = replay from oldest retained,
    * the original behavior). */
   char id[128];
   snprintf(id, sizeof(id), "%s", id_in);
   uint64_t cursor = 0;
   char *q = strchr(id, '?');
   if (q)
   {
      *q = '\0';
      const char *c = strstr(q + 1, "cursor=");
      if (c)
         cursor = (uint64_t)strtoull(c + 7, NULL, 10);
   }

   char probe[80];
   if (presence_session_json(id, probe, sizeof(probe)) == 0)
   {
      send_response(fd, 404, "{\"error\":\"no such session\"}", request_id);
      return;
   }
   write_sse_headers(fd, request_id);
   char *data = (char *)malloc(PRESENCE_EVENT_DATA_MAX + 1);
   if (!data)
      return; /* headers already sent; just drop the stream */
   char ev[PRESENCE_EVENT_NAME_MAX];
   for (;;)
   {
      presence_wait_t w =
          presence_wait(id, &cursor, 1000, ev, sizeof(ev), data, PRESENCE_EVENT_DATA_MAX + 1);
      if (w == PRESENCE_WAIT_EVENT)
      {
         /* Emit the post-advance cursor as the SSE id so the client can persist
          * it and resume from exactly here after a reconnect/remount (?cursor=). */
         char idline[40];
         int idn = snprintf(idline, sizeof(idline), "id: %llu\n", (unsigned long long)cursor);
         if (idn > 0 && write_all_fd(fd, idline, idn) < 0)
            break;
         if (ev[0])
         {
            if (write_all_fd(fd, "event: ", 7) < 0)
               break;
            if (write_all_fd(fd, ev, (int)strlen(ev)) < 0)
               break;
            if (write_all_fd(fd, "\n", 1) < 0)
               break;
         }
         if (write_all_fd(fd, "data: ", 6) < 0)
            break;
         if (write_all_fd(fd, data, (int)strlen(data)) < 0)
            break;
         if (write_all_fd(fd, "\n\n", 2) < 0)
            break;
         continue;
      }
      if (w == PRESENCE_WAIT_GONE)
         break;
      /* PRESENCE_WAIT_TIMEOUT: heartbeat; a failed write means the client
       * disconnected, so stop streaming and free the listener. */
      if (write_all_fd(fd, ": keep-alive\n\n", 13) < 0)
         break;
   }
   free(data);
}
