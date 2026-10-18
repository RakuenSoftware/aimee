/* server_http_op_runs.c: owned async dispatch-backed operation lifecycle. */
#include "aimee.h"
#include "server_http_internal.h"
#include "server_http_identity.h"
#include "agent_config.h"
#include "compute_pool.h"
#include "openai_runs_store.h"
#include "roundtable_pipeline_capture.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── async op runner ──────────────────────────────────────────────────────────
 * Long-running / LLM methods (kb.build, graph.sync_code, memory.benchmark,
 * curator.synthesize, …) cannot use rh_dispatch_op: the inline bridge would run
 * them on the single listener thread and stall all /v1 serving. rh_dispatch_op_async
 * instead enqueues the dispatch onto a detached worker, returns a `queued` run
 * handle immediately, and the worker drives the loopback RPC to completion and
 * finalizes the record. Status/result are polled via GET /v1/runs/{id} (the run
 * store is generic — it holds whatever JSON snapshot we store). Capability is the
 * row's op-derived cap, gated before we enqueue. */
typedef struct
{
   char run_id[128];
   char op[64];
   char *line;         /* dispatch body with method injected; owned by worker */
   uint32_t conn_caps; /* caps captured at enqueue time */
   long created;
   request_context_snapshot_t request_context;
} op_run_job_t;

static void op_run_job_free(op_run_job_t *job)
{
   if (!job)
      return;
   request_context_snapshot_clear(&job->request_context);
   free(job->line);
   free(job);
}

/* Build the run snapshot object stored for GET /v1/runs/{id}. `result` is an
 * already-serialized JSON value (object/array/string) or NULL while queued. */
static char *op_run_snapshot(const char *run_id, const char *op, const char *status, long created,
                             const char *result)
{
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return NULL;
   cJSON_AddStringToObject(o, "id", run_id);
   cJSON_AddStringToObject(o, "object", "op.run");
   cJSON_AddStringToObject(o, "method", op);
   cJSON_AddStringToObject(o, "status", status);
   cJSON_AddNumberToObject(o, "created", (double)created);
   if (result)
   {
      cJSON *r = cJSON_Parse(result);
      cJSON_AddItemToObject(o, "result", r ? r : cJSON_CreateString(result));
   }
   char *s = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   return s;
}

/* Orchestration workers are pooled. Every field below is thread-local, but that
 * only prevents cross-thread races: without an explicit turn boundary, the next
 * op scheduled on the same worker inherits the previous op's checkout and
 * credential context. In particular, an interactive roundtable could make a
 * server-hosted Codex seat `cd` into another client's nonexistent checkout and
 * return no content. Clear on both sides of every op dispatch so request-local
 * panel/preset/artifact state cannot bleed through worker reuse. */
static void op_run_clear_thread_context(void)
{
   aimee_ir_session_begin_task();
   server_http_identity_clear();
   request_context_clear();
   run_cmd_set_cwd(NULL);
   agent_set_request_session(NULL);
   agent_set_request_codex_creds(NULL, NULL);
   agent_set_request_vault_principal(NULL);
   agent_set_request_cancel(NULL);
}

static void op_run_worker_run(void *arg)
{
   op_run_job_t *j = (op_run_job_t *)arg;
   op_run_clear_thread_context();
   request_context_restore(&j->request_context);
   server_http_identity_bind_request_owner(request_context_get());
   agent_set_request_vault_principal(
       j->request_context.active ? j->request_context.value.vault_principal : NULL);
   compute_pool_set_job(POOL_JOB_DELEGATE, "op=%s run=%s", j->op, j->run_id);
   openai_runs_store_set_status(j->run_id, OPENAI_RUN_IN_PROGRESS);
   char *started = op_run_snapshot(j->run_id, j->op, "in_progress", j->created, NULL);
   if (started)
   {
      openai_runs_store_update_json(j->run_id, started);
      free(started);
   }

   char *buf = (char *)malloc(SHTTP_RESP_MAX);
   if (!buf)
   {
      char *snap =
          op_run_snapshot(j->run_id, j->op, "failed", j->created, "{\"error\":\"out of memory\"}");
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED, snap ? snap : "{}");
      free(snap);
      op_run_clear_thread_context();
      op_run_job_free(j);
      compute_pool_clear_job();
      return;
   }

   int rc = loopback_rpc(j->line, (int)strlen(j->line), buf, SHTTP_RESP_MAX, j->conn_caps);
   /* A dispatch returning a JSON object with "error" (or a non-2xx rc) is a
    * failed run; anything else is the completed result payload. */
   int ok = (rc >= 200 && rc < 300);
   openai_run_status_t terminal_status = OPENAI_RUN_COMPLETED;
   if (ok)
   {
      cJSON *parsed = cJSON_Parse(buf);
      if (parsed)
      {
         if (cJSON_GetObjectItemCaseSensitive(parsed, "error"))
            ok = 0;
         cJSON *dispatch_status = cJSON_GetObjectItemCaseSensitive(parsed, "status");
         if (cJSON_IsString(dispatch_status) && strcmp(dispatch_status->valuestring, "error") == 0)
            ok = 0;
         cJSON *cancelled = cJSON_GetObjectItemCaseSensitive(parsed, "cancelled");
         if (cJSON_IsTrue(cancelled))
            terminal_status = OPENAI_RUN_CANCELLED;
         cJSON_Delete(parsed);
      }
   }
   const char *status_str =
       terminal_status == OPENAI_RUN_CANCELLED ? "cancelled" : (ok ? "completed" : "failed");
   char *snap = op_run_snapshot(j->run_id, j->op, status_str, j->created, buf);
   openai_runs_store_finalize(j->run_id, ok ? terminal_status : OPENAI_RUN_FAILED,
                              snap ? snap : buf);
   /* Pipeline-owned result capture (#18): persist the terminal attempt into the
    * durable DB1 ledger before the bounded /v1/runs record can be evicted. This
    * no-ops for ordinary (non-pipeline) op-runs. */
   rtp_seam_finalize(j->run_id, ok, terminal_status == OPENAI_RUN_CANCELLED, buf);
   free(snap);
   free(buf);
   op_run_clear_thread_context();
   op_run_job_free(j);
   compute_pool_clear_job();
}

static int submit_op_run_internal(const char *op_method, const char *body_json, uint32_t conn_caps,
                                  int preflight_caps, char *resp, int cap)
{
   if (!op_method || !op_method[0])
      return err_json(resp, cap, 500, "route has no dispatch method");
   const request_context_t *owner = request_context_get();
   if (owner && owner->vault_transport != ATTEST_NONE)
      conn_caps &= owner->capabilities;
   uint32_t required = server_capability_for_method(op_method);
   if (preflight_caps && required && (conn_caps & required) == 0)
      return err_json(resp, cap, 403, "forbidden: insufficient capabilities");
   cJSON *req = (body_json && body_json[0]) ? cJSON_Parse(body_json) : cJSON_CreateObject();
   if (!req || !cJSON_IsObject(req))
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   cJSON_DeleteItemFromObjectCaseSensitive(req, "method");
   cJSON_AddStringToObject(req, "method", op_method);

   long created = (long)time(NULL);
   static atomic_ulong g_op_run_seq = 0;
   char id[128];
   unsigned long seq = atomic_fetch_add_explicit(&g_op_run_seq, 1, memory_order_relaxed) + 1;
   snprintf(id, sizeof(id), "oprun_%s_%ld_%lu", openai_runs_store_generation(), created, seq);

   cJSON_DeleteItemFromObjectCaseSensitive(req, "__run_id");
   cJSON_AddStringToObject(req, "__run_id", id);

   /* Pipeline-owned submission seam (#20/#21): a validated `pipeline_pass_id`
    * registers an attempt row keyed to this run id and is forwarded internally
    * as the private `__pipeline_pass_id`. A caller-supplied double-underscore id
    * is never honored. An unknown / non-owned pass id is rejected. */
   cJSON_DeleteItemFromObjectCaseSensitive(req, "__pipeline_pass_id");
   cJSON *ppid = cJSON_GetObjectItemCaseSensitive(req, "pipeline_pass_id");
   if (ppid && cJSON_IsNumber(ppid))
   {
      int pass_id = (int)ppid->valuedouble;
      cJSON_DeleteItemFromObjectCaseSensitive(req, "pipeline_pass_id");
      if (rtp_seam_register_attempt(pass_id, id) < 0)
      {
         cJSON_Delete(req);
         return err_json(resp, cap, 403, "invalid or non-owned pipeline_pass_id");
      }
      cJSON_AddNumberToObject(req, "__pipeline_pass_id", pass_id);
   }

   char *line = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!line)
      return err_json(resp, cap, 500, "out of memory");

   char *queued = op_run_snapshot(id, op_method, "queued", created, NULL);
   if (!queued || !openai_runs_store_create(id, queued))
   {
      free(queued);
      free(line);
      return err_json(resp, cap, 500, "could not create run");
   }

   op_run_job_t *j = (op_run_job_t *)calloc(1, sizeof(*j));
   if (!j)
   {
      free(queued);
      free(line);
      return err_json(resp, cap, 500, "out of memory");
   }
   snprintf(j->run_id, sizeof(j->run_id), "%s", id);
   snprintf(j->op, sizeof(j->op), "%s", op_method);
   j->line = line; /* ownership moves to the worker */
   request_context_snapshot(&j->request_context);
   if (j->request_context.active)
      j->request_context.value.capabilities = conn_caps;
   j->conn_caps = conn_caps;
   j->created = created;

   server_ctx_t *ctx = server_active_ctx();
   /* Every async operation has the same server-owned lifecycle. Keeping them in
    * one dedicated pool prevents a newly registered coordinator from silently
    * falling back to the generic compute lane, and makes shutdown authoritative. */
   if (ctx && ctx->orchestration_pool_initialized)
   {
      int submit_rc = compute_pool_submit(&ctx->orchestration_pool, op_run_worker_run, j);
      if (submit_rc != COMPUTE_POOL_SUBMIT_OK)
      {
         const char *message = submit_rc == COMPUTE_POOL_SUBMIT_CLOSED ? "orchestration unavailable"
                                                                       : "orchestration queue full";
         char detail[96];
         snprintf(detail, sizeof(detail), "{\"error\":\"%s\"}", message);
         char *failed = op_run_snapshot(id, op_method, "failed", created, detail);
         openai_runs_store_finalize(id, OPENAI_RUN_FAILED, failed ? failed : queued);
         free(failed);
         op_run_job_free(j);
         free(queued);
         return err_json(resp, cap, 503, message);
      }
   }
   else
   {
      char *failed = op_run_snapshot(id, op_method, "failed", created,
                                     "{\"error\":\"orchestration unavailable\"}");
      openai_runs_store_finalize(id, OPENAI_RUN_FAILED, failed ? failed : queued);
      free(failed);
      op_run_job_free(j);
      free(queued);
      return err_json(resp, cap, 503, "orchestration unavailable");
   }

   int n = snprintf(resp, (size_t)cap, "%s", queued);
   free(queued);
   return (n > 0 && n < cap) ? 200 : 200;
}

int server_http_submit_op_run(const char *op_method, const char *body_json, uint32_t conn_caps,
                              char *resp, int cap)
{
   return submit_op_run_internal(op_method, body_json, conn_caps, 1, resp, cap);
}

int rh_dispatch_op_async(const route_req_t *rq, char *resp, int cap)
{
   return submit_op_run_internal(rq->op, rq->body, g_rpc_conn_caps, 0, resp, cap);
}
