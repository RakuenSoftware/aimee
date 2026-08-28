/* sandbox_audit_bridge.c: forwards sandbox degraded-isolation events onto the
 * audit event bus. The one place that links sandbox.c to the (D7-confined) audit
 * bus — see sandbox_audit_bridge.h. Only non-secret fields cross here; the raw
 * command line (which may carry secrets in its arguments) is never in scope — the
 * hook already reduced it to a bare program name. */
#include "sandbox_audit_bridge.h"

#include <stdio.h>

#include <aimee/audit/audit_action.h> /* audit_args_hash, AUDIT_ARGS_HASH_LEN */
#include <aimee/audit/obs_bus.h>      /* obs_bus_emit */
#include "sandbox.h"                  /* sandbox_audit_hook_fn, sandbox_mode_to_string */

/* sandbox_audit_hook_fn: publish one degraded-isolation audit row over the bus
 * (KIND_AUDIT_ACTION -> the audit ledger + capture/replay). NON-SECRET only:
 * actor "sandbox" (a subsystem event, no principal at this primitive), tool
 * "sandbox.exec", the program name (command), the requested isolation (mode, with
 * a "+netiso" suffix when network isolation was requested), the availability
 * reason, and the verdict ("unsandboxed_fallback" / "refused"). No task
 * association -> task_id 0. */
static void on_sandbox_degraded(const char *program, sandbox_mode_t mode, int network_isolated,
                                const char *verdict, const char *reason)
{
   char modestr[48];
   snprintf(modestr, sizeof modestr, "%s%s", sandbox_mode_to_string(mode),
            network_isolated ? "+netiso" : "");

   /* Keyed per-op fingerprint, uniform with the other audit rows; the program
    * identity is carried human-readable in the command field. */
   char args_hash[AUDIT_ARGS_HASH_LEN];
   snprintf(args_hash, sizeof args_hash, "v1-");
   audit_args_hash("sandbox.exec", NULL, args_hash, sizeof args_hash);

   if (obs_bus_commit_action("sandbox", "sandbox.exec", args_hash, program ? program : "", modestr,
                             reason ? reason : "", verdict, /*task_id=*/0) != 0)
      fprintf(stderr, "audit: sandbox WORM commit failed\n");
   obs_bus_emit("sandbox", "sandbox.exec", args_hash, program ? program : "", modestr,
                reason ? reason : "", verdict, /*task_id=*/0);
}

void sandbox_audit_bridge_install(void)
{
   sandbox_set_audit_hook(on_sandbox_degraded);
}
