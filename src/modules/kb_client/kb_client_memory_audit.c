/* kb_client_memory_audit.c: the server-side memory-mutation audit hook plumbing.
 * Kept in its own dependency-free translation unit (no RPC/HTTP/cJSON) so the
 * bridge->bus->ledger test can link the seam without dragging in the whole
 * kb_client stack. See kb_client.h for the contract. */
#include "kb_client.h"

#include <stddef.h>

/* Installed once at startup by the server-only bridge (NULL = no audit). */
static kb_client_memory_audit_hook_fn g_mem_audit_hook = NULL;

void kb_client_set_memory_audit_hook(kb_client_memory_audit_hook_fn fn)
{
   g_mem_audit_hook = fn;
}

void kb_client_memory_audit_note(const char *op, int64_t id, const char *tier, const char *kind,
                                 const char *key, double confidence, const char *session_id, int ok)
{
   kb_client_memory_audit_hook_fn h = g_mem_audit_hook;
   if (!h)
      return;
   h(op, id, tier ? tier : "", kind ? kind : "", key ? key : "", confidence,
     session_id ? session_id : "", ok);
}
