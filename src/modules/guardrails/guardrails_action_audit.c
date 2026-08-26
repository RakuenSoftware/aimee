/* guardrails_action_audit.c: the per-action governed-action audit (P2 / S2).
 *
 * pre_tool_check is a thin wrapper around the verdict logic
 * (pre_tool_check_inner in guardrails_orchestrator.c): it emits EXACTLY ONE
 * audit row per call, AFTER the verdict is decided, so the many inner return
 * paths cannot drift the emit count. Required audit failures fail the action
 * closed; a governed effect must never outrun its evidence. */
#include <string.h>

#include <stdio.h>

#include "aimee.h" /* MODE_APPROVE */
#include <aimee/audit/audit_action.h>
#include <aimee/audit/obs_bus.h> /* the per-action row now crosses the event bus, not a direct write */
#include <aimee/audit/audit_worm.h>
#include "config.h"
#include "guardrails.h"
#include "log.h"

/* Cached audit_action_enabled. Read once on first use; a legacy_config_read failure
 * leaves the memset-zeroed flag at 0 (audit OFF), so the gate is fail-safe and
 * the hot path costs one branch instead of a per-call config parse. Toggling the
 * knob takes effect on restart — acceptable for a passive, default-on audit. */
static int g_audit_action_enabled = -1;
static int audit_action_is_enabled(void)
{
   if (g_audit_action_enabled < 0)
   {
      g_audit_action_enabled = config_audit_action_enabled() ? 1 : 0;
   }
   return g_audit_action_enabled;
}

/* Cached required WORM posture. The accessor is default-on and accepts only the
 * deployment-visible break-glass override; ordinary config loss/edit cannot
 * silently disable governed-action capture. */
static int g_audit_worm_enabled = -1;
static int audit_worm_is_enabled(void)
{
   if (g_audit_worm_enabled < 0)
   {
      g_audit_worm_enabled = config_audit_worm_enabled() ? 1 : 0;
   }
   return g_audit_worm_enabled;
}

/* Clear both cached gates so the next audit call re-reads config. Runs after a
 * reload publishes the new snapshot (registered via config_reload_register_
 * reapplier), so a config.set / SIGHUP applies live instead of needing a restart. */
static void audit_gate_reload_reapplier(void)
{
   g_audit_action_enabled = -1;
   g_audit_worm_enabled = -1;
}

void guardrails_action_audit_register_reload(void)
{
   config_reload_register_reapplier(audit_gate_reload_reapplier);
}

/* Dual-write the same governed-action row into the append-only WORM store. S0:
 * best-effort — a failure is recoverable audit loss (audit.log stays
 * authoritative) and never touches the verdict. Structured principal/detail
 * schemas and fail-closed authority arrive in later slices. */
static int emit_worm_row(const char *actor, const char *tool_name, const char *args_hash,
                         const char *mode, const char *reason, const char *verdict,
                         long long task_id)
{
   if (!audit_worm_is_enabled())
      return 0;
   char action[192];
   snprintf(action, sizeof action, "tool.%s", tool_name ? tool_name : "");
   char detail[320];
   snprintf(detail, sizeof detail, "{\"mode\":\"%s\",\"reason\":\"%s\",\"task_id\":%lld}",
            mode ? mode : "", reason ? reason : "", task_id);
   return audit_worm_append(actor, "", action, args_hash ? args_hash : "", verdict ? verdict : "",
                            detail);
}

static int emit_action_audit(const char *tool_name, const char *input_json,
                             const char *guardrail_mode, session_state_t *state, int rc,
                             const char *msg_buf)
{
   /* reason_code = the stable key of the block site's existing audit_log() call,
    * captured on this thread (see audit_last_event). Empty for allow/rewrite. */
   const char *reason = audit_last_event();

   const char *verdict;
   if (rc == 2)
   {
      /* rc==2 covers hard block and computer-use approval-required. The block
       * sites carry an audit key; computer-use uses an "APPROVAL_REQUIRED:"
       * msg prefix (classification only — the prose is never persisted). */
      int approval = (reason && strstr(reason, "approval")) ||
                     (msg_buf && strncmp(msg_buf, "APPROVAL_REQUIRED", 17) == 0);
      verdict = approval ? "approval_required" : "block";
   }
   else if (rc == 1 || rc == 3)
      verdict = "rewrite";
   else
      verdict = rc == 0 ? "allow" : "unknown"; /* surface novel rc, don't hide it */

   if (!reason || !reason[0])
      reason = verdict;

   const char *sid = session_id();
   if (!sid || !sid[0])
   {
      LOG_ERROR("audit", "governed action refused: session identity unavailable");
      return -1;
   }
   char actor[96];
   snprintf(actor, sizeof(actor), "%s:%s", (state && state->is_delegate) ? "delegate" : "primary",
            sid);
   const char *mode = guardrail_mode ? guardrail_mode : MODE_APPROVE;
   char args_hash[AUDIT_ARGS_HASH_LEN];
   if (audit_args_hash(tool_name, input_json, args_hash, sizeof args_hash) != 0)
   {
      LOG_ERROR("audit", "governed action refused: argument hash unavailable for tool=%s",
                tool_name ? tool_name : "");
      return -1;
   }
   /* Arg-free command preview (shell tools only; "" otherwise). Safe-by-
    * construction — only program basenames, never an argument value — so it
    * rides on the same audit_action_enabled gate with no extra PII surface. */
   char command[288];
   audit_command_preview(tool_name, input_json, command, sizeof command);
   long long task_id = state ? (long long)state->active_task_id : 0;
   /* The governed-action row now goes over the event bus (delivery step 3): this
    * emit publishes the row; a consumer thread performs the real ledger append via
    * audit_action_log. The direct call is gone — the bus is the sole route (an
    * all-or-nothing migration, no flagged parallel write). Still off the verdict's
    * critical path and best-effort: a publish failure never blocks the tool. */
   /* The ordinary observability stream remains operator-configurable.  The WORM
    * decision record below is an independent, required enforcement precondition;
    * disabling the convenience action log must never disable durable evidence. */
   if (audit_action_is_enabled())
      obs_bus_emit(actor, tool_name, args_hash, command, mode, reason, verdict, task_id);
   if (emit_worm_row(actor, tool_name, args_hash, mode, reason, verdict, task_id) != 0)
   {
      LOG_ERROR("audit", "governed action refused: WORM append failed for tool=%s",
                tool_name ? tool_name : "");
      return -1;
   }
   return 0;
}

int pre_tool_check(const char *tool_name, const char *input_json, session_state_t *state,
                   const char *guardrail_mode, const char *cwd, char *msg_buf, size_t msg_len)
{
   /* Clear the last audit event before the verdict so a block site's key from a
    * prior call cannot leak as this call's reason_code. */
   audit_last_event_reset();
   int rc =
       pre_tool_check_inner(tool_name, input_json, state, guardrail_mode, cwd, msg_buf, msg_len);
   if (emit_action_audit(tool_name, input_json, guardrail_mode, state, rc, msg_buf) != 0 && rc != 2)
   {
      if (msg_buf && msg_len > 0)
         snprintf(msg_buf, msg_len,
                  "Blocked: required governed-action audit evidence could not be committed.");
      return 2;
   }
   return rc;
}
