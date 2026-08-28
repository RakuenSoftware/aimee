/* kb_mcp_audit_bridge.c: forwards each kb-hosted MCP plugin tool-call OUTCOME onto
 * aimee-kb's OWN observability bus. The one place linking the kb's MCP execution
 * seam to the bus; kb_service_agent.c (the caller) stays bus-free. Mirrors
 * kb_memory_audit_bridge.c.
 *
 * Only NON-CONTENT fields cross. The (tool) identity is reduced to a name-only
 * fingerprint via audit_args_hash(tool, NULL, ...) — exactly as the vault / sandbox
 * / memory / server tool-completion bridges do — so no argument or result content,
 * and no plugin-returned error text, ever reaches the ledger. The kb records the
 * outcome of tools it hosts (install: kb), the mirror of the server-side tool-call
 * audit for the server-hosted case. */
#include "kb_mcp_audit_bridge.h"

#include <stdio.h>

#include <aimee/audit/audit_action.h> /* audit_args_hash, AUDIT_ARGS_HASH_LEN */
#include <aimee/audit/obs_bus.h>      /* obs_bus_emit (lazy-starts the bus on first emit) */

void kb_mcp_audit_record(const char *actor, const char *tool, const char *mode,
                         const char *reason_code, const char *verdict)
{
   if (!tool || !tool[0])
      return;

   /* Name-only fingerprint, uniform with the other audit rows; args_json is NULL so
    * no argument content is serialized or hashed. audit_args_hash fully initializes
    * the buffer (including its own "v1-" prefix). */
   char args_hash[AUDIT_ARGS_HASH_LEN];
   audit_args_hash(tool, NULL, args_hash, sizeof args_hash);

   const char *safe_actor = actor && actor[0] ? actor : "mcp";
   const char *safe_mode = mode && mode[0] ? mode : "outbound";
   const char *safe_reason = reason_code ? reason_code : "";
   const char *safe_verdict = verdict && verdict[0] ? verdict : "ok";
   if (obs_bus_commit_action(safe_actor, tool, args_hash, /*command=*/"", safe_mode, safe_reason,
                             safe_verdict, /*task_id=*/0) != 0)
      fprintf(stderr, "audit: MCP action WORM commit failed for %s\n", tool);
   obs_bus_emit(safe_actor, tool, args_hash, /*command=*/"", safe_mode, safe_reason, safe_verdict,
                /*task_id=*/0);
}
