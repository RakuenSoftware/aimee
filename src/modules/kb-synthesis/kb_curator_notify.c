/* kb_curator_notify.c: outbound invalidation push. See header. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_notify.h"
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "platform_ipc.h"
#include "kb_http_ws.h"

#include <string.h>
#include <unistd.h>

#define CURATOR_NOTIFY_TIMEOUT_MS 500

int kb_curator_notify_send(const char *notify_socket, const char *source_kind,
                           const char *source_id, int artifacts_stale)
{
   if (!notify_socket || !notify_socket[0])
      return 0; /* disabled */

   int fd = platform_ipc_connect(notify_socket, CURATOR_NOTIFY_TIMEOUT_MS);
   if (fd < 0)
   {
      aimee_log(LOG_DEBUG, "kb.curator.notify", "connect to %s failed; dropping event",
                notify_socket);
      return -1;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "curator.invalidated");
   cJSON_AddStringToObject(req, "source_kind", source_kind ? source_kind : "");
   cJSON_AddStringToObject(req, "source_id", source_id ? source_id : "");
   cJSON_AddNumberToObject(req, "artifacts_stale", artifacts_stale);
   char *line = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!line)
   {
      close(fd);
      return -1;
   }

   int rc = 0;
   size_t len = strlen(line);
   /* request + newline (the server frames messages on '\n'). */
   if (write(fd, line, len) != (ssize_t)len || write(fd, "\n", 1) != 1)
      rc = -1;
   free(line);
   close(fd); /* best-effort: do not wait for / read the response */
   if (rc == 0)
      aimee_log(LOG_DEBUG, "kb.curator.notify", "pushed invalidation for %s (%d stale)",
                source_id ? source_id : "", artifacts_stale);
   return rc;
}

int kb_curator_notify_invalidation(const char *source_kind, const char *source_id,
                                   int artifacts_stale)
{
   config_t cfg;
   config_load(&cfg);
   return kb_curator_notify_send(cfg.kb_curator_invalidation_notify_socket, source_kind, source_id,
                                 artifacts_stale);
}

void kb_curator_invalidation_broadcast(const char *project, const char *file_path,
                                       int artifacts_stale)
{
   if (artifacts_stale <= 0)
      return;
   /* Real-time RPC push to a subscribed aimee-server (best-effort). */
   (void)kb_curator_notify_invalidation("kb_file", file_path, artifacts_stale);
   /* In-process /v1/events WebSocket fan-out so subscribed clients (the
    * aimee-server cache subscriber, IDEs) invalidate on signal. */
   kb_ws_publish_invalidation("doc", "project", project);
}
