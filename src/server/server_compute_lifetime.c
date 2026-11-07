/* Compute context owns its cancellation-safe source and budget lifetime. */
#include "server_compute_internal.h"
#include "server_compute_impl.h"
#include "config.h"
#include "agent_tasks.h"
#include "modules/workspace/workspace_turn.h"
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef AIMEE_POSIX
#include <unistd.h>
#endif

void compute_ctx_release_budget(compute_ctx_t *cctx)
{
   if (!cctx || cctx->compute_grant <= 0)
      return;

   if (g_aimee_compute_threads_override == cctx->compute_grant)
      g_aimee_compute_threads_override = 0;
   if (cctx->compute_budget_acquired || cctx->compute_executor_threads <= 0)
      server_compute_budget_release(cctx->server, cctx->compute_grant);
   cctx->compute_grant = 0;
   cctx->compute_budget_acquired = 0;
}

static _Thread_local compute_ctx_t **compute_lifetime_slot;
compute_ctx_t **compute_ctx_lifetime_bind(compute_ctx_t **slot)
{
   compute_ctx_t **previous = compute_lifetime_slot;
   compute_lifetime_slot = slot;
   return previous;
}

void compute_ctx_free(compute_ctx_t *cctx)
{
   if (!cctx)
      return;
   int previous_cancel;
   pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel);
   if (compute_lifetime_slot && *compute_lifetime_slot == cctx)
      *compute_lifetime_slot = NULL;
   if (cctx->original_submission && !cctx->original_admitted)
      server_chat_ir_abort(cctx);
   else
      server_chat_ir_close(cctx);
   if (cctx->original_submission)
   {
      cJSON *raw = cJSON_GetObjectItemCaseSensitive(cctx->req, "message");
      if (cJSON_IsString(raw))
         OPENSSL_cleanse(raw->valuestring, strlen(raw->valuestring));
   }
   host_original_submission_finish(cctx->original_submission, cctx->original_admitted);
   host_original_submission_destroy(&cctx->original_submission);
   host_original_submission_result_destroy(&cctx->original_result);
   aimee_message_free(&cctx->ir_input);
   aimee_request_free(&cctx->delegate_request);
   free(cctx->delegate_task);
   if (cctx->background_job_id > 0)
      agent_set_durable_job(0);
   if (cctx->compute_grant > 0)
   {
      compute_ctx_release_budget(cctx);
   }
   if (cctx->req)
      cJSON_Delete(cctx->req);
   free(cctx->delta_buf); /* presence-ring text-coalescing buffer (WP-1) */
   free(cctx->live_text); /* db1 live-turn mirror accumulator */
   if (cctx->write_mutex)
   {
      pthread_mutex_destroy(cctx->write_mutex);
      free(cctx->write_mutex);
   }
#ifdef AIMEE_POSIX
   if (cctx->conn_fd >= 0)
      close(cctx->conn_fd);
#endif
   request_context_snapshot_clear(&cctx->request_context);
   const workspace_turn_mirror_lease_t *bound = workspace_turn_mirror_bind(NULL);
   if (bound != cctx->source_mirror)
      workspace_turn_mirror_bind(bound);
   workspace_turn_mirror_release(cctx->source_mirror);
   free(cctx);
   pthread_setcancelstate(previous_cancel, NULL);
}
