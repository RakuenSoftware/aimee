/* test_curator_notify.c: outbound invalidation push sender — disabled and
 * unreachable-socket paths (a live server is validated at deploy). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kb_curator_notify.h"
#include "kb_http_ws.h"

/* Recording stand-in for the real /v1/events publisher (defined in
 * kb_http_ws.c, not linked into this unit test): lets us observe that the
 * broadcast helper fans out to the WebSocket transport. */
static int g_ws_pub_calls = 0;
static char g_ws_kind[32], g_ws_scope_kind[32], g_ws_scope_id[64];
void kb_ws_publish_invalidation(const char *kind, const char *scope_kind, const char *scope_id)
{
   g_ws_pub_calls++;
   snprintf(g_ws_kind, sizeof(g_ws_kind), "%s", kind ? kind : "");
   snprintf(g_ws_scope_kind, sizeof(g_ws_scope_kind), "%s", scope_kind ? scope_kind : "");
   snprintf(g_ws_scope_id, sizeof(g_ws_scope_id), "%s", scope_id ? scope_id : "");
}

int main(void)
{
   /* No notify socket configured -> disabled, no-op, success. */
   assert(kb_curator_notify_send("", "kb_file", "f.md", 3) == 0);
   assert(kb_curator_notify_send(NULL, "kb_file", "f.md", 3) == 0);

   /* Configured but unreachable -> best-effort failure, never blocks. */
   assert(kb_curator_notify_send("/tmp/aimee-curator-notify-nonexistent.sock", "kb_file", "f.md",
                                 1) == -1);

   printf("  notify_send disabled + unreachable paths OK\n");

   /* broadcast with 0 stale artifacts is a no-op (no RPC, no WS publish). */
   g_ws_pub_calls = 0;
   kb_curator_invalidation_broadcast("proj-a", "f.md", 0);
   assert(g_ws_pub_calls == 0);
   /* broadcast with >0 stale publishes exactly one doc/project WS event. */
   g_ws_pub_calls = 0;
   kb_curator_invalidation_broadcast("proj-a", "f.md", 2);
   assert(g_ws_pub_calls == 1);
   assert(strcmp(g_ws_kind, "doc") == 0);
   assert(strcmp(g_ws_scope_kind, "project") == 0);
   assert(strcmp(g_ws_scope_id, "proj-a") == 0);
   printf("  broadcast fan-out: 0-stale no-op, >0 publishes doc/project event OK\n");

   printf("curator_notify: all tests passed\n");
   return 0;
}
