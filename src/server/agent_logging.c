/* agent_logging.c: agent_log + token_audit recording.
 *
 * Extracted from agent_runtime.c (which sits at the line-count limit). Owns the
 * per-call audit write shared by the internal agent path (agent_log_call) and the
 * ingress handlers (agent_record_token_audit), plus the per-thread ingress-source
 * override used by ingress workers such as /v1/runs. */
#include "aimee.h"
#include "agent_exec.h"
#include "config.h" /* session_id() */
#include "db1.h"    /* token_audit + agent_log inserts */
#include "token_tracker.h"

#include <stdio.h>
#include <string.h>

/* Exported by server_compute when the call happens inside a delegate worker;
 * weak-stub returns NULL elsewhere (CLI, tests). */
const char *delegation_active_id(void);

/* Per-thread ingress source: the /v1/runs worker sets this so spend logged via
 * agent_log_call (source="agent") from inside the run loop is retagged with the
 * ingress origin instead — distinguishing it from internal execution, no 2nd row.
 * The runs worker is a dedicated detached thread, so it is scoped to that turn. */
static __thread char g_ingress_source[40] = "";

/* The agent_log row id for the call currently being logged, set by agent_log_call
 * so the token_audit row it writes links back 1:1. 0 outside agent_log_call (e.g.
 * direct ingress writes, which have no agent_log row). */
static __thread long long g_agent_log_id = 0;

void agent_set_ingress_source(const char *source)
{
   snprintf(g_ingress_source, sizeof(g_ingress_source), "%s", source ? source : "");
}

void agent_record_token_audit(const agent_result_t *result, const char *role, const char *source)
{
   if (!result)
      return;

   /* A thread-scoped ingress source overrides the caller's source (see above). */
   const char *eff_source = g_ingress_source[0] ? g_ingress_source : (source ? source : "");

   token_usage_t usage = {
       .input_tokens = result->prompt_tokens,
       .output_tokens = result->completion_tokens,
       .cache_write_tokens = result->cache_write_tokens,
       .cache_read_tokens = result->cache_read_tokens,
   };
   /* Bill against the model actually served to the provider, not the agent
    * identity: an agent named "codex" may serve "gpt-5.4", and a turn-0 400 may
    * have swapped in the fallback model. Fall back to the agent name only when
    * no served model was recorded (e.g. the dedup cache-hit path). */
   const char *bill_model = result->model[0] ? result->model : result->agent_name;
   double cost = token_estimate_cost(bill_model, &usage);
   /* Tagging the audit row with the active delegation id lets cost-fold attribute
    * a child's spend back to the parent without contaminating session_id sums. */
   const char *deleg_id = delegation_active_id();
   db1_token_audit_row_t row = {
       .session_id = session_id(),
       .delegation_id = deleg_id ? deleg_id : "",
       .project_name = "",
       .tool_name = result->agent_name,
       .role = role ? role : "",
       /* The served model (consistent with the cost key above), so the by-model
        * breakdown attributes spend to the real model rather than the agent. */
       .model = bill_model,
       .source = eff_source,
       .requested_model = result->requested_model,
       .stop_reason = result->stop_reason,
       .agent_log_id = g_agent_log_id,
       .prompt_tokens = usage.input_tokens,
       .completion_tokens = usage.output_tokens,
       .cache_write_tokens = usage.cache_write_tokens,
       .cache_read_tokens = usage.cache_read_tokens,
       .estimated_cost_usd = cost,
   };
   (void)db1_token_audit_insert(&row);
}

void agent_log_call(const agent_result_t *result, const char *role)
{
   db1_agent_log_insert_row_t row = {
       .agent_name = result->agent_name,
       .role = role ? role : "",
       .prompt_tokens = result->prompt_tokens,
       .completion_tokens = result->completion_tokens,
       .latency_ms = result->latency_ms,
       .success = result->success,
       .error = result->error[0] ? result->error : NULL,
       .turns = result->turns,
       .tool_calls = result->tool_calls,
       .confidence = result->confidence,
       .session_id = NULL,
   };
   long long log_id = db1_agent_log_insert(&row);

   /* Link the cost row to this agent_log row (1:1) for agent stats, then clear. */
   g_agent_log_id = log_id > 0 ? log_id : 0;
   /* Internal agent/delegate execution; tag the cost row accordingly. */
   agent_record_token_audit(result, role, "agent");
   g_agent_log_id = 0;
}
