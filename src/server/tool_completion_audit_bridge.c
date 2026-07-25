/* tool_completion_audit_bridge.c: forwards each completed tool dispatch's OUTCOME
 * onto the audit event bus. The one place that links the tools dispatcher to the
 * (D7-confined) audit bus — see tool_completion_audit_bridge.h.
 *
 * The dispatcher already reduced the call to a fixed verdict / reason_code enum
 * and a coarse mode before firing the hook, so NO argument, result, or MCP-server
 * error text reaches this bridge — only classified identity. In particular the
 * hook contract forbids the raw err_buf an MCP server returned from ever becoming
 * reason_code; it is always one of the enum values. */
#include "tool_completion_audit_bridge.h"

#include <stdio.h>

#include <aimee/audit/audit_action.h> /* audit_args_hash, AUDIT_ARGS_HASH_LEN */
#include <aimee/audit/obs_bus.h>      /* obs_bus_emit */

#include "config.h"      /* MAX_PATH_LEN, transitively needed by agent_types.h */
#include "agent_tools.h" /* agent_tool_completion_t, agent_tools_set_tool_completion_cb */

/* Publish one completion row (KIND_AUDIT_ACTION -> the audit ledger +
 * capture/replay). Fields, mapped onto obs_bus_emit's fixed eight:
 *   actor       the principal captured on the dispatch thread (session id / role)
 *   tool        the tool name (namespaced for a remote tool) — length-clamped by
 *               obs_bus's put_str, bounded and non-content
 *   args_hash   a name-only identity token (audit_args_hash with NULL args, exactly
 *               as the vault/sandbox/memory bridges do) — carries no argument content
 *   command     empty: there is no non-secret command line at this seam
 *   mode        "internal" / "outbound" / "served"
 *   reason_code the outcome class enum (never free text)
 *   verdict     "ok" / "error" / "timeout" / "refused"
 *   task_id     0 — correlation is by actor + tool with the pre-check row */
static void on_tool_completion(const char *tool, const agent_tool_completion_t *o, void *ud)
{
   (void)ud;
   if (!tool || !o)
      return;

   /* Name-only fingerprint, uniform with the other audit rows; args_json is NULL
    * so no argument content is serialized or hashed (audit_args_hash would ignore
    * it for a non-allowlisted MCP tool anyway). */
   char args_hash[AUDIT_ARGS_HASH_LEN];
   snprintf(args_hash, sizeof args_hash, "v1-");
   audit_args_hash(tool, NULL, args_hash, sizeof args_hash);

   obs_bus_emit(o->actor && o->actor[0] ? o->actor : "tool", tool, args_hash, /*command=*/"",
                o->mode && o->mode[0] ? o->mode : "internal", o->reason_code ? o->reason_code : "",
                o->verdict && o->verdict[0] ? o->verdict : "ok",
                /*task_id=*/0);
}

void tool_completion_audit_bridge_install(void)
{
   agent_tools_set_tool_completion_cb(on_tool_completion, NULL);
}
