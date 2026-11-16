/* server_compute_owner.c: immutable request owner across compute handoffs. */
#include "server_compute_impl.h"
#include "db1_client/agent_jobs.h"
#include "db1_client/coord_jobs.h"
#include "server_coord_dispatcher.h"
#include "config.h"
#include "liveness.h"
#include "log.h"
#include <aimee/ir/session_event.h>
#include <stdlib.h>
#include "server_http_identity.h"
#include "agent_config.h"
#include "server_session_control.h"
#include <stdio.h>
#include <string.h>

void compute_ctx_capture_request_context(compute_ctx_t *cctx, const server_conn_t *conn)
{
   request_context_snapshot(&cctx->request_context);
   request_context_snapshot_t *snapshot = &cctx->request_context;
   request_context_t *owner = &snapshot->value;
   server_control_scope_t *controls = server_control_scope_current();
   if (snapshot->active && controls)
      owner->control_reply_capability =
          owner->control_reply_capability && server_control_scope_can_reply(controls);
   /* A conn-free continuation inherits its actual owner, not CAPS_ALL. Only
    * genuinely context-free server work retains the historical server scope. */
   int inherited_owner = snapshot->active && owner->vault_transport != ATTEST_NONE;
   cctx->conn_caps = conn ? conn->capabilities : inherited_owner ? owner->capabilities : CAPS_ALL;
   if (conn && conn->attested_transport != ATTEST_NONE)
   {
      if (snapshot->active && (owner->vault_transport != conn->attested_transport ||
                               strcmp(owner->vault_principal, conn->vault_principal)))
         request_context_snapshot_clear(snapshot);
      snapshot->active = 1;
      int local = conn->attested_transport == ATTEST_UDS_PEERCRED ||
                  conn->attested_transport == ATTEST_WEBCHAT_TRUSTED;
      owner->peer_uid = local ? (long)conn->peer_uid : -1;
      owner->transport = local ? REQ_TRANSPORT_UDS : REQ_TRANSPORT_TCP;
      owner->vault_transport = conn->attested_transport;
      snprintf(owner->vault_principal, sizeof owner->vault_principal, "%s", conn->vault_principal);
   }
   else if (conn && inherited_owner)
      cctx->conn_caps &= owner->capabilities;
   owner->capabilities = cctx->conn_caps;
   cctx->attested_transport = snapshot->active ? owner->vault_transport : ATTEST_NONE;
   snprintf(cctx->vault_principal, sizeof cctx->vault_principal, "%s",
            snapshot->active && owner->vault_transport != ATTEST_NONE ? owner->vault_principal
            : (conn && conn->vault_principal[0])                      ? conn->vault_principal
                                                 : agent_get_request_vault_principal());
}

void compute_ctx_bind_request_context(const compute_ctx_t *cctx)
{
   request_context_restore(cctx ? &cctx->request_context : NULL);
   server_http_identity_bind_request_owner(request_context_get());
   agent_set_request_vault_principal(cctx ? cctx->vault_principal : NULL);
}
void compute_ctx_clear_request_context(void)
{
   agent_set_request_session(NULL);
   agent_set_request_codex_creds(NULL, NULL);
   server_http_identity_clear();
   request_context_clear();
   agent_set_request_vault_principal(NULL);
}

/* Durable terminal state is a projection of admitted model IR plus the host's
 * execution outcome. Compatibility display strings are never evidence of work. */
typedef struct
{
   const char *status;
   const char *result;
   char *display;
   char diagnostic[2048];
} compute_terminal_outcome_t;

static void compute_terminal_outcome(const cJSON *resp, compute_terminal_outcome_t *out)
{
   memset(out, 0, sizeof *out);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resp, "status"));
   const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resp, "message"));
   out->status = status && !strcmp(status, "cancelled") ? "cancelled" : "failed";
   snprintf(out->diagnostic, sizeof out->diagnostic,
            "delegate did not return a complete canonical answer");
   out->result = message && message[0] ? message : out->diagnostic;
   aimee_response_t ir = {0};
   if (aimee_ir_response_parse(cJSON_GetObjectItemCaseSensitive(resp, "ir"), &ir))
      return; /* A pre-provider host failure need not have model IR. */
   out->display = aimee_ir_response_text_alloc(&ir);
   if (!out->display)
      goto finish;

   int authored = 0, pending_tool = 0, media = 0;
   for (int i = 0; i < ir.n_content; i++)
   {
      const aimee_block_t *block = &ir.content[i];
      media |= block->type == AIMEE_BLK_IMAGE || block->type == AIMEE_BLK_DOCUMENT;
      authored |= (block->type == AIMEE_BLK_TEXT && block->text && block->text[0]) ||
                  block->type == AIMEE_BLK_IMAGE || block->type == AIMEE_BLK_DOCUMENT;
      pending_tool |= block->type == AIMEE_BLK_TOOL_USE || block->type == AIMEE_BLK_TOOL_RESULT;
   }
   if (status && !strcmp(status, "ok") && !aimee_ir_response_complete_valid(&ir))
   {
      const cJSON *turns = cJSON_GetObjectItemCaseSensitive(resp, "turns");
      const cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tool_calls");
      int unexecuted = (!cJSON_IsNumber(turns) || turns->valueint == 0) &&
                       (!cJSON_IsNumber(tools) || tools->valueint == 0) &&
                       liveness_is_unexecuted_tool_plan_response(out->display);
      if (!media && (liveness_is_degenerate_response(out->display) || unexecuted))
         snprintf(out->diagnostic, sizeof out->diagnostic,
                  unexecuted
                      ? "delegate returned an unexecuted tool-use plan without tool execution"
                      : "delegate returned raw tool-call markup or another degenerate response");
      else
      {
         out->status = "done";
         out->result = out->display;
      }
   }
   else if (status && !strcmp(status, "error") && ir.stop_reason == AIMEE_STOP_MAX_TOKENS &&
            !ir.error_message && !ir.refusal && !pending_tool && authored)
   {
      out->status = "partial";
      snprintf(out->diagnostic, sizeof out->diagnostic,
               "Partial result; delegate ended with error: %.500s\n\n%.1300s",
               message && message[0] ? message : "model token limit reached", out->display);
      out->result = out->diagnostic;
   }
   else if ((!message || !message[0]) && ir.error_message && ir.error_message[0])
      snprintf(out->diagnostic, sizeof out->diagnostic, "%s", ir.error_message);
finish:
   aimee_response_free(&ir);
}

void compute_ctx_update_background_job(compute_ctx_t *cctx, cJSON *resp)
{
   if (!cctx || cctx->background_job_id <= 0 || !resp)
      return;
   compute_terminal_outcome_t outcome;
   compute_terminal_outcome(resp, &outcome);
   cJSON *turns = cJSON_GetObjectItemCaseSensitive(resp, "turns");
   cJSON *cost_usd = cJSON_GetObjectItemCaseSensitive(resp, "cost_usd");
   int cursor_turn = 0;
   if (cJSON_IsNumber(turns))
      cursor_turn = turns->valueint;
   else
   {
      db1_agent_job_t job;
      if (db1_agent_job_get(cctx->background_job_id, &job) == 0)
         cursor_turn = job.cursor_turn;
      db1_agent_job_free(&job); /* zero-init by get on miss; safe either way */
   }

   cJSON *cost_known = cJSON_GetObjectItemCaseSensitive(resp, "cost_known");
   /* An absent or false cost_known means the delegate produced no measurement.
    * Leave the stored cost unknown rather than recording a false zero. */
   int has_cost =
       cJSON_IsTrue(cost_known) && cJSON_IsNumber(cost_usd) && cost_usd->valuedouble >= 0.0;
   if (db1_agent_job_complete(cctx->background_job_id, outcome.status, cursor_turn, outcome.result,
                              has_cost, has_cost ? cost_usd->valuedouble : 0.0) != 0)
      aimee_log(LOG_ERROR, "delegate", "background job %d terminal status/cost write failed",
                cctx->background_job_id);
   free(outcome.display);
}

void compute_ctx_update_coord_task(compute_ctx_t *cctx, cJSON *resp)
{
   if (!cctx || cctx->coord_task_id <= 0 || !resp)
      return;
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resp, "status"));
   if (status && !strcmp(status, "preempted"))
   {
      if (db1_coord_job_release_task_bounded_owned(cctx->coord_task_id, cctx->coord_claim_owner,
                                                   config_concurrency_preempt_requeue_max()) == 0)
      {
         server_coord_dispatcher_notify();
         return;
      }
      db1_coord_job_fail_task_owned(cctx->coord_task_id, cctx->coord_claim_owner,
                                    "preempt requeue cap exhausted");
   }
   else
   {
      compute_terminal_outcome_t outcome;
      compute_terminal_outcome(resp, &outcome);
      if (!strcmp(outcome.status, "done"))
         db1_coord_job_complete_task_owned(cctx->coord_task_id, cctx->coord_claim_owner,
                                           outcome.result);
      else
         db1_coord_job_fail_task_owned(cctx->coord_task_id, cctx->coord_claim_owner,
                                       outcome.result);
      free(outcome.display);
   }
   server_coord_dispatcher_notify();
}
