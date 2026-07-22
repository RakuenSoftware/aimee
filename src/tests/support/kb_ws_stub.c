/* kb_ws_stub.c: no-op kb_ws_publish_invalidation for unit tests that link the
 * kb HTTP handlers (ingest/releases) without the full WebSocket module and its
 * OpenSSL/pthread/jobs dependency tree. The handlers call this on success; for
 * a handler unit test the broadcast is irrelevant. */
#include "kb_http_ws.h"

void kb_ws_publish_invalidation(const char *kind, const char *scope_kind, const char *scope_id)
{
   (void)kind;
   (void)scope_kind;
   (void)scope_id;
}

/* Upgrade-detection no-ops for tests that link kb_http_conn.o (the plain-HTTP
 * connection handler probes for a WS upgrade before routing). */
int kb_ws_is_upgrade(const char *req_buf)
{
   (void)req_buf;
   return 0;
}

int kb_ws_is_ws_path(const char *clean_path)
{
   (void)clean_path;
   return 0;
}

void kb_ws_spawn(int fd, const char *req_buf, const char *clean_path)
{
   (void)fd;
   (void)req_buf;
   (void)clean_path;
}
