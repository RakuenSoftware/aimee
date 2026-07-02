/* guardrails_action_audit.c: the per-action governed-action audit (P2 / S2).
 *
 * pre_tool_check is a thin wrapper around the verdict logic
 * (pre_tool_check_inner in guardrails_orchestrator.c): it emits EXACTLY ONE
 * audit row per call, AFTER the verdict is decided, so the many inner return
 * paths cannot drift the emit count. The emit is strictly side-effect-only —
 * any failure here (hashing, log write) leaves the verdict untouched (audit loss
 * is acceptable, enforcement drift is not). Kept in its own file so the verdict
 * logic stays within the source line budget. */
#include <string.h>

#include "aimee.h" /* MODE_APPROVE */
#include "audit_action.h"
#include "config.h"
#include "guardrails.h"
#include "log.h"

/* Cached audit_action_enabled. Read once on first use; a config_load failure
 * leaves the memset-zeroed flag at 0 (audit OFF), so the gate is fail-safe and
 * the hot path costs one branch instead of a per-call config parse. Toggling the
 * knob takes effect on restart — acceptable for a passive, default-on audit. */
static int g_audit_action_enabled = -1;
static int audit_action_is_enabled(void)
{
   if (g_audit_action_enabled < 0)
   {
      config_t cfg;
      memset(&cfg, 0, sizeof cfg);
      config_load(&cfg);
      g_audit_action_enabled = cfg.audit_action_enabled ? 1 : 0;
   }
   return g_audit_action_enabled;
}

static void emit_action_audit(const char *tool_name, const char *input_json,
                              const char *guardrail_mode, session_state_t *state, int rc,
                              const char *msg_buf)
{
   if (!audit_action_is_enabled())
      return;

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

   const char *actor = (state && state->is_delegate) ? "delegate" : "primary";
   const char *mode = guardrail_mode ? guardrail_mode : MODE_APPROVE;
   /* Pre-init to the S1 sentinel so the wrapper is safe even if audit_args_hash
    * changes behavior; audit_args_hash also writes the sentinel first. */
   char args_hash[AUDIT_ARGS_HASH_LEN];
   snprintf(args_hash, sizeof args_hash, "v1-");
   audit_args_hash(tool_name, input_json, args_hash, sizeof args_hash);
   long long task_id = state ? (long long)state->active_task_id : 0;
   audit_action_log(actor, tool_name, args_hash, mode, reason, verdict, task_id);
}

int pre_tool_check(const char *tool_name, const char *input_json, session_state_t *state,
                   const char *guardrail_mode, const char *cwd, char *msg_buf, size_t msg_len)
{
   /* Clear the last audit event before the verdict so a block site's key from a
    * prior call cannot leak as this call's reason_code. */
   audit_last_event_reset();
   int rc =
       pre_tool_check_inner(tool_name, input_json, state, guardrail_mode, cwd, msg_buf, msg_len);
   emit_action_audit(tool_name, input_json, guardrail_mode, state, rc, msg_buf);
   return rc;
}
