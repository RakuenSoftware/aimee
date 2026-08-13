/* server_compute.c: compute-layer handlers (tool.execute, delegate, chat.send_stream) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_compute_internal.h"
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "db1.h"
#include "server_delegate_monitor.h" /* delegate heartbeat begin/end (keep slow delegates alive) */
#include "server_compute_impl.h"
#include "agent_config.h"
#include <aimee/gateway/gateway_policy.h>
#include "presence.h"
#include "compute_pool.h"
#include "agent.h"
#include "agent_coord.h"
#include "cmd_agent_delegate_impl.h"
#include "config.h"
#include "token_tracker.h"
#include <aimee/delegates/delegate_credential_retry.h>
#include <aimee/delegates/delegate_launch.h>
#include <aimee/delegates/delegate_sandbox_image.h>
#include <aimee/delegates/delegate_source_authority.h>
#include "agent_source_authority.h" /* TLS source-authority context (race-free in-process) */
#include "server_coord_dispatcher.h"
#include <aimee/delegates/delegate_credentials.h>
#include "vault_service.h" /* WP-C.1 vault-first credential resolution */
#include <openssl/crypto.h>
#include <aimee/delegates/delegate_economics.h>
#include <aimee/delegates/delegate_run_phases.h>
#include "db1/delegate_learning.h"
#include "db1/delegate_reservation.h"
#include "request_context.h" /* execution key carried as the idempotency key */
#include "kb_client.h"
#include "kb_bandit.h"
#include "db1/interaction_events.h"
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/delegate_role.h>
#include "delegate_ensemble.h"
#include "evidence_replay.h"
#include <aimee/delegates/delegate_ephemeral_ws.h>
#include "guardrails.h"
#include "liveness.h"
#include "log.h"
#include "model_registry.h"
#include "openai_runs_store.h"
#include "platform_process.h"
#include "prompts.h"
#include <limits.h>
#include "persona.h"
#include "roundtable_preset.h"
#include "server_http.h"
#include "provider_catalog.h"
#include "role_templates.h"
#include <aimee/workspace/workspace.h>
#include "modules/workspace/workspace_provider.h"
#include "modules/workspace/workspace_turn.h"
#include "cJSON.h"
#include "dstr.h"

/* Defined in agent_runtime_tmux.c (no shared header). Drives the standard CLI
 * agent over a tmux session, which runs on the client over the reverse channel
 * when the active workspace is detached. */
int agent_execute_cli_session(const agent_t *agent, const agent_network_t *network,
                              const char *system_prompt, const char *user_prompt, int max_tokens,
                              double temperature, agent_result_t *out);
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* Delegation mailbox: allows delegates to pause and receive parent replies */

delegation_mailbox_t g_mailboxes[MAX_ACTIVE_DELEGATIONS];
pthread_mutex_t g_mailbox_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_delegate_id_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_delegate_id_seq = 0;
static void delegate_generate_id(char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   unsigned long seq;
   pthread_mutex_lock(&g_delegate_id_lock);
   seq = ++g_delegate_id_seq;
   pthread_mutex_unlock(&g_delegate_id_lock);
   struct timespec ts;
   if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
   {
      ts.tv_sec = time(NULL);
      ts.tv_nsec = 0;
   }
   snprintf(out, cap, "deleg-%d-%lld%09ld-%lu", (int)getpid(), (long long)ts.tv_sec, ts.tv_nsec,
            seq);
}

/* compute_ctx_t is defined in server_compute_impl.h */

void compute_ctx_begin_budget(compute_ctx_t *cctx)
{
   if (!cctx || cctx->compute_grant > 0)
      return;
   if (cctx->compute_executor_threads > 0)
   {
      cctx->compute_grant = cctx->compute_executor_threads;
      cctx->compute_budget_acquired = 0;
      g_aimee_compute_threads_override = cctx->compute_grant;
      return;
   }
   cctx->compute_grant = server_compute_budget_acquire(cctx->server);
   cctx->compute_budget_acquired = 1;
   g_aimee_compute_threads_override = cctx->compute_grant;
}

/* Write all data to fd, handling non-blocking with poll */
int write_all(int fd, const char *data, size_t len)
{
   size_t total = 0;
   while (total < len)
   {
      ssize_t n = write(fd, data + total, len - total);
      if (n > 0)
      {
         total += (size_t)n;
      }
      else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
         if (platform_wait_writable(fd) != 0)
            return -1;
      }
      else
      {
         return -1;
      }
   }
   return 0;
}

static void compute_update_background_job(compute_ctx_t *cctx, cJSON *resp)
{
   if (!cctx || cctx->background_job_id <= 0 || !resp)
      return;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *turns = cJSON_GetObjectItemCaseSensitive(resp, "turns");
   cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(resp, "tool_calls");
   cJSON *response = cJSON_GetObjectItemCaseSensitive(resp, "response");
   cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
   cJSON *cost_usd = cJSON_GetObjectItemCaseSensitive(resp, "cost_usd");
   int has_response = cJSON_IsString(response) && response->valuestring[0];
   int has_message = cJSON_IsString(message) && message->valuestring[0];

   const char *job_status = "failed";
   if (cJSON_IsString(status))
   {
      if (strcmp(status->valuestring, "ok") == 0)
         job_status = "done";
      else if (strcmp(status->valuestring, "cancelled") == 0)
         job_status = "cancelled";
      else if (strcmp(status->valuestring, "error") == 0 && has_response)
         job_status = "partial";
   }

   const char *result = NULL;
   char partial_result[2048];
   partial_result[0] = '\0';
   if (cJSON_IsString(response))
      result = response->valuestring;
   else if (cJSON_IsString(message))
      result = message->valuestring;
   if (strcmp(job_status, "partial") == 0 && has_message)
   {
      snprintf(partial_result, sizeof(partial_result),
               "Partial result; delegate ended with error: %.500s\n\n%.1300s", message->valuestring,
               response->valuestring);
      result = partial_result;
   }
   else if (strcmp(job_status, "done") == 0 && has_response &&
            liveness_is_degenerate_response(response->valuestring))
   {
      snprintf(partial_result, sizeof(partial_result),
               "delegate returned raw tool-call markup or another degenerate response");
      job_status = "failed";
      result = partial_result;
   }
   else if (strcmp(job_status, "done") == 0 && has_response &&
            (!cJSON_IsNumber(turns) || turns->valueint == 0) &&
            (!cJSON_IsNumber(tool_calls) || tool_calls->valueint == 0) &&
            liveness_is_unexecuted_tool_plan_response(response->valuestring))
   {
      snprintf(partial_result, sizeof(partial_result),
               "delegate returned an unexecuted tool-use plan without tool execution");
      job_status = "failed";
      result = partial_result;
   }

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
   if (db1_agent_job_complete(cctx->background_job_id, job_status, cursor_turn, result, has_cost,
                              has_cost ? cost_usd->valuedouble : 0.0) != 0)
      aimee_log(LOG_ERROR, "delegate", "background job %d terminal status/cost write failed",
                cctx->background_job_id);
}

static void compute_update_coord_task(compute_ctx_t *cctx, cJSON *resp)
{
   if (!cctx || cctx->coord_task_id <= 0 || !resp)
      return;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   const char *status_text = cJSON_IsString(status) ? status->valuestring : "";
   if (strcmp(status_text, "ok") == 0)
   {
      cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
      db1_coord_job_complete_task_owned(cctx->coord_task_id, cctx->coord_claim_owner,
                                        cJSON_IsString(r) ? r->valuestring : "");
   }
   else if (strcmp(status_text, "preempted") == 0)
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
      cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
      db1_coord_job_fail_task_owned(cctx->coord_task_id, cctx->coord_claim_owner,
                                    (cJSON_IsString(m) && m->valuestring[0]) ? m->valuestring
                                                                             : "delegate failed");
   }
   server_coord_dispatcher_notify();
}

/* Write response and free context */
void compute_respond(compute_ctx_t *cctx, cJSON *resp)
{
   compute_update_background_job(cctx, resp);
   compute_update_coord_task(cctx, resp);

   if (cctx->conn_fd < 0)
   {
      cJSON_Delete(resp);
      return;
   }

   char *json_str = cJSON_PrintUnformatted(resp);
   if (json_str)
   {
      size_t len = strlen(json_str);
      if (cctx->write_mutex)
         pthread_mutex_lock(cctx->write_mutex);
      if (write_all(cctx->conn_fd, json_str, len) == 0)
         write_all(cctx->conn_fd, "\n", 1);
      if (cctx->write_mutex)
         pthread_mutex_unlock(cctx->write_mutex);
      free(json_str);
   }
   cJSON_Delete(resp);
}

void compute_error(compute_ctx_t *cctx, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   compute_respond(cctx, resp);
}

void compute_ok(compute_ctx_t *cctx)
{
   cJSON *resp = jo_ok();
   compute_respond(cctx, resp);
}

/* Build an error string augmented with delegation retry guidance, if any of
 * the known error patterns match. Falls back to the raw message otherwise.
 * Writes into out (NUL-terminated). */
static void delegation_augment_error(const char *message, char *out, size_t out_cap)
{
   if (!out || out_cap == 0)
      return;
   if (!message)
   {
      out[0] = '\0';
      return;
   }
   char guidance[512];
   if (delegation_error_guidance(message, guidance, sizeof(guidance)) && guidance[0])
      snprintf(out, out_cap, "%s%s", message, guidance);
   else
      snprintf(out, out_cap, "%s", message);
}

/* compute_error variant for delegation paths: appends actionable retry
 * guidance to the message when it matches a known delegation error pattern,
 * so any consumer (in-process or external MCP) gets the fix hint without
 * each transport having to re-run the matcher. */
static void delegation_compute_error(compute_ctx_t *cctx, const char *message)
{
   char buf[2048];
   delegation_augment_error(message, buf, sizeof(buf));
   compute_error(cctx, buf);
}

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

void compute_ctx_free(compute_ctx_t *cctx)
{
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
   free(cctx);
}

/* --- delegate worker --- */

void delegate_worker(void *arg);

/* Test hook: when non-NULL, replaces the production pthread/pool dispatch.
 * Production never sets this; tests use it to keep delegate_worker on the
 * caller thread so they can drive it synchronously. */
int (*g_delegate_dispatch_override)(compute_ctx_t *cctx) = NULL;

static const char *compute_request_session_id(cJSON *req)
{
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (cJSON_IsString(jsid) && jsid->valuestring[0])
      return jsid->valuestring;
   jsid = cJSON_GetObjectItemCaseSensitive(req, "aimee_session_id");
   if (cJSON_IsString(jsid) && jsid->valuestring[0])
      return jsid->valuestring;
   jsid = cJSON_GetObjectItemCaseSensitive(req, "claude_session_id");
   if (cJSON_IsString(jsid) && jsid->valuestring[0])
      return jsid->valuestring;
   jsid = cJSON_GetObjectItemCaseSensitive(req, "provider_session_id");
   if (cJSON_IsString(jsid) && jsid->valuestring[0])
      return jsid->valuestring;
   return NULL;
}

/* Bind this request's per-turn creds (RAM-keyring session id + Codex creds),
 * shared by every delegate entry so single delegate and aggregate/roundtable
 * panels resolve client-held keys identically; without it a panel runs keyless. */
static void bind_request_session_creds(cJSON *req)
{
   const char *cred_sid = jo_str(req, "cred_session_id", NULL);
   agent_set_request_session((cred_sid && cred_sid[0]) ? cred_sid
                                                       : compute_request_session_id(req));
   /* Clear any per-turn codex creds carried on this pooled thread; the vault is the
    * source now (delegate_credential_retry sets them from the vault), and the
    * client no longer pushes a codex token in the request body (P4b). */
   agent_set_request_codex_creds(NULL, NULL);
}

/* On-demand delegate execution: server_delegate_ondemand.c. */
static int delegate_dispatch(server_ctx_t *ctx, compute_ctx_t *cctx)
{
   if (g_delegate_dispatch_override)
      return g_delegate_dispatch_override(cctx);

   if (!ctx)
      return -1;

   const char *sid = cctx ? compute_request_session_id(cctx->req) : NULL;
   if (sid && sid[0])
   {
      int session_threads = 0;
      cctx->compute_executor_threads =
          ctx->session_threads > 0 ? ctx->session_threads : CONFIG_DEFAULT_SESSION_THREADS;
      int rc = server_session_pool_submit(ctx, sid, delegate_worker, cctx, &session_threads);
      if (rc != 0)
         cctx->compute_executor_threads = 0;
      return rc;
   }

   /* Sessionless/background delegates run on their own on-demand thread, not the
    * CPU compute pool (server_delegate_ondemand.c). */
   return delegate_spawn_ondemand(cctx);
}

static void delegate_request_parent_context(cJSON *jdepth, cJSON *jparent, int *depth_out,
                                            const char **parent_out)
{
   int depth = cJSON_IsNumber(jdepth) ? (int)jdepth->valuedouble : 0;
   const char *parent =
       cJSON_IsString(jparent) && jparent->valuestring[0] ? jparent->valuestring : NULL;

   if (parent)
   {
      if (!db1_delegation_spawn_is_active(parent))
      {
         parent = NULL;
         depth = 0;
      }
      else if (depth <= 0)
      {
         depth = 1;
      }
   }

   if (depth < 0)
      depth = 0;
   *depth_out = depth;
   *parent_out = parent;
}

/* The caller-context a delegate run mutates and must restore exactly: the
 * thread-local delegation depth/parent, their AIMEE_DELEGATE_DEPTH /
 * AIMEE_PARENT_DELEGATION_ID env mirror (for cross-process child clients), and
 * the source-authority env snapshot. delegate_run_ctx_enter saves the current
 * values and installs this run's; delegate_run_ctx_restore puts them back. Both
 * the normal teardown and the concurrency-reject early return call restore, so
 * the save/restore lives in one place instead of being copied per exit. */
typedef struct
{
   delegation_mailbox_t *mb;
   int saved_depth;
   char saved_parent[64];
   char saved_env_depth[32];
   char saved_env_parent[64];
   delegate_source_env_snapshot_t source_env;
   agent_source_authority_snapshot_t sa_snap;
} delegate_run_ctx_t;

static void delegate_run_ctx_enter(delegate_run_ctx_t *c, const char *deleg_id, const char *sid,
                                   int current_depth, const char *source_env_root)
{
   c->mb = mailbox_acquire(deleg_id);
   tl_mailbox = c->mb;
   session_id_set_override(sid);

   c->saved_depth = tl_delegation_depth;
   snprintf(c->saved_parent, sizeof(c->saved_parent), "%s", tl_parent_delegation_id);
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "%s", deleg_id);
   tl_delegation_depth = current_depth;

   const char *e = getenv("AIMEE_DELEGATE_DEPTH");
   c->saved_env_depth[0] = '\0';
   if (e && e[0])
      snprintf(c->saved_env_depth, sizeof(c->saved_env_depth), "%s", e);
   char depth_str[32];
   snprintf(depth_str, sizeof(depth_str), "%d", current_depth);
   platform_setenv("AIMEE_DELEGATE_DEPTH", depth_str);

   e = getenv("AIMEE_PARENT_DELEGATION_ID");
   c->saved_env_parent[0] = '\0';
   if (e && e[0])
      snprintf(c->saved_env_parent, sizeof(c->saved_env_parent), "%s", e);
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", deleg_id);

   /* Env (process-global) is kept for cross-process child clients that inherit
    * it. The in-process source-authority consumer (code_search overlay) instead
    * reads the thread-local context below, which does NOT race across the
    * concurrent delegate threads the way the shared env does. clear_for_worktree
    * disables authority + clears paths, so mirror that: authority=0, paths=NULL,
    * worktree=source_env_root. */
   delegate_source_env_capture(&c->source_env);
   delegate_source_env_clear_for_worktree(source_env_root);
   agent_source_authority_tls_capture(&c->sa_snap);
   agent_source_authority_tls_set(0, source_env_root, NULL);
}

static void delegate_run_ctx_restore(delegate_run_ctx_t *c)
{
   agent_source_authority_tls_restore(&c->sa_snap);
   platform_setenv("AIMEE_DELEGATE_DEPTH", c->saved_env_depth[0] ? c->saved_env_depth : "");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", c->saved_env_parent[0] ? c->saved_env_parent : "");
   delegate_source_env_restore(&c->source_env);
   tl_delegation_depth = c->saved_depth;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "%s", c->saved_parent);

   session_id_clear_override();
   tl_mailbox = NULL;
   mailbox_release(c->mb);
}

/* Export this delegate's context (depth/parent/source-authority) from the
 * forking thread's TLS into the child's environment. Call ONLY in a freshly
 * fork()ed child, before exec: the parent-side process-global AIMEE_DELEGATE_*
 * env races across concurrent delegate threads, so a cross-process sub-client
 * (a CLI agent that shells out to `aimee delegate`) could inherit a neighbor
 * delegate's depth/parent. fork() copies the forking thread's TLS into the
 * (now single-threaded) child, so re-deriving the env here is immune to that
 * clobber. No-op for primary agents (depth 0, no parent), which keep their
 * inherited environment. Matches the adjacent post-fork unsetenv() pattern. */
void delegate_child_export_context_env(void)
{
   if (tl_delegation_depth > 0 || tl_parent_delegation_id[0])
   {
      char depth_str[32];
      snprintf(depth_str, sizeof(depth_str), "%d", tl_delegation_depth);
      platform_setenv("AIMEE_DELEGATE_DEPTH", depth_str);
      platform_setenv("AIMEE_PARENT_DELEGATION_ID", tl_parent_delegation_id);
   }
   agent_source_authority_export_env();
}

/* Build the delegate result envelope from a finished run. Mirrors the two
 * branches the worker emitted inline: on success the response + turn/token
 * metrics; on failure the (possibly stop-reason) status + augmented message,
 * with the response and apply_error attached when present. Economics and
 * handoff-validation fields are added the same way for both. Returns a new
 * cJSON object; the caller attaches checkout info and responds. */
/* Publish the realized cost only when the audit actually recorded one. A
 * missing audit row means the spend is unknown, not zero: the consumer must be
 * able to tell those apart or unmeasured provider spend is committed as free. */
static void delegate_add_measured_cost_json(cJSON *resp, const char *deleg_id)
{
   double cost = 0.0;
   int known = db1_token_audit_cost_for_delegation_ex(deleg_id, &cost) == 0;
   cJSON_AddBoolToObject(resp, "cost_known", known);
   if (known)
      cJSON_AddNumberToObject(resp, "cost_usd", cost);
}

static cJSON *delegate_build_result_response(
    const char *deleg_id, int rc, const agent_result_t *result, const agent_config_t *acfg,
    const char *role, const agent_t *target_agent, int applied_changes, int handoff_checked,
    const delegate_handoff_validation_t *handoff_validation, const char *apply_error)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "delegation_id", deleg_id);
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "response", result->response ? result->response : "");
      cJSON_AddNumberToObject(resp, "turns", result->turns);
      cJSON_AddNumberToObject(resp, "tool_calls", result->tool_calls);
      cJSON_AddNumberToObject(resp, "confidence", result->confidence);
      cJSON_AddNumberToObject(resp, "latency_ms", result->latency_ms);
      cJSON_AddStringToObject(resp, "agent", result->agent_name);
      cJSON_AddNumberToObject(resp, "prompt_tokens", result->prompt_tokens);
      cJSON_AddNumberToObject(resp, "completion_tokens", result->completion_tokens);
      delegate_economics_add_agent_result_json(resp, acfg, role, result, target_agent);
      delegate_add_measured_cost_json(resp, deleg_id);
      if (applied_changes >= 0)
         cJSON_AddNumberToObject(resp, "applied_changes", applied_changes);
      if (handoff_checked)
         delegate_handoff_add_validation_json(resp, handoff_validation);
   }
   else
   {
      char stop_reason[32] = "";
      const char *err_status = "error";
      if (db1_delegation_spawn_stop_reason(deleg_id, stop_reason, sizeof(stop_reason)) == 1)
         err_status = stop_reason;
      cJSON_AddStringToObject(resp, "status", err_status);
      char augmented[2048];
      delegation_augment_error(result->error[0] ? result->error : "delegation failed", augmented,
                               sizeof(augmented));
      cJSON_AddStringToObject(resp, "message", augmented);
      cJSON_AddNumberToObject(resp, "turns", result->turns);
      cJSON_AddNumberToObject(resp, "tool_calls", result->tool_calls);
      cJSON_AddNumberToObject(resp, "latency_ms", result->latency_ms);
      cJSON_AddStringToObject(resp, "agent", result->agent_name);
      cJSON_AddNumberToObject(resp, "prompt_tokens", result->prompt_tokens);
      cJSON_AddNumberToObject(resp, "completion_tokens", result->completion_tokens);
      delegate_economics_add_agent_result_json(resp, acfg, role, result, target_agent);
      delegate_add_measured_cost_json(resp, deleg_id);
      if (result->response)
         cJSON_AddStringToObject(resp, "response", result->response);
      if (apply_error[0])
         cJSON_AddStringToObject(resp, "apply_error", apply_error);
      if (handoff_checked)
         delegate_handoff_add_validation_json(resp, handoff_validation);
   }
   return resp;
}

/* Append an owned context `block` to the delegate system prompt, taking ownership
 * (frees `block` and, on a successful concat, the previous template buffer).
 * No-op when block is NULL. Shared by the code-context / graph-context / tier
 * injectors so the ownership dance lives in one place. */
static void delegate_append_owned_block(const char **system_prompt, char **template_sys_prompt,
                                        char *block)
{
   if (!block)
      return;
   char *combined = delegate_prompt_append_block(*system_prompt, block);
   if (combined)
   {
      free(*template_sys_prompt);
      *template_sys_prompt = combined;
      *system_prompt = combined;
   }
   free(block);
}

/* Convert a USD ceiling into a conservative total-token ceiling after routing,
 * when the actual model and authoritative DB1 pricing are known. Charging one
 * synthetic token in every usage class deliberately overstates the per-token
 * rate (notably for cached input), which makes the resulting token ceiling a
 * safety bound rather than a spend prediction. */
static int delegate_apply_cost_limit(agent_t *agent, double max_cost_usd, int input_tokens,
                                     int *max_tokens, char *err, size_t err_cap)
{
   if (max_cost_usd <= 0.0)
      return 0;
   if (!agent || !agent->model[0])
   {
      snprintf(err, err_cap, "max_cost_usd requires a routed model");
      return -1;
   }
   int token_cap = 0, total_cap = 0;
   int ceiling = token_cost_ceiling(agent->model, max_cost_usd, input_tokens, *max_tokens,
                                    &token_cap, &total_cap);
   if (ceiling < 0)
   {
      snprintf(err, err_cap, "cannot enforce max_cost_usd %.6f for model '%s'", max_cost_usd,
               agent->model);
      return -1;
   }
   if (ceiling == 0) /* explicitly free model */
      return 0;
   *max_tokens = token_cap;
   if (agent->max_tokens <= 0 || agent->max_tokens > token_cap)
      agent->max_tokens = token_cap;
   if (agent->middleware.cost_limit <= 0 || agent->middleware.cost_limit > total_cap)
      agent->middleware.cost_limit = total_cap;
   return 0;
}

void delegate_worker(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   compute_ctx_begin_budget(cctx);
   if (cctx->background_job_id > 0)
   {
      char lease_owner[32];
      snprintf(lease_owner, sizeof(lease_owner), "%d", (int)getpid());
      if (db1_agent_job_take_lease(cctx->background_job_id, lease_owner) != 0)
      {
         if (!db1_agent_job_is_cancelled(cctx->background_job_id))
            db1_agent_job_update(cctx->background_job_id, "failed", 0,
                                 "failed to take delegate job lease");
         compute_ctx_free(cctx);
         return;
      }
      agent_set_durable_job(cctx->background_job_id);
   }
   /* Cleanup-relevant state, hoisted + zero-initialised so the single
    * delegate_fail: error path releases exactly what was acquired — each
    * cleanup action is guarded by its own zero value, so an early exit that
    * never set these is a no-op. (This also fixes pre-existing credential-lease
    * leaks at the depth/spawn-limit exits, which returned without releasing.) */
   char deleg_id[64] = "";
   agent_t *target_agent = NULL;
   char leased_cred_name[MAX_CRED_NAME_LEN] = "";
   /* WP-C.3: the credential-pool row key. "" for the shared env pool; the request
    * principal for a vaulted credential, so its 429-cooldown is per-principal. */
   char leased_principal[VAULT_PRINCIPAL_MAX] = "";
   char credential_state_path[MAX_PATH_LEN] = "";
   char *resolved_prompt = NULL;
   char *template_sys_prompt = NULL;
   char delegate_worktree_path[MAX_PATH_LEN] = "";
   char delegate_git_root[MAX_PATH_LEN] = "";
   char delegate_work_name[32] = "";
   /* The turn's root when it is NOT the directory the caller named -- a
    * server-side reconstruction of a client workspace. Declared here, ahead of
    * every `goto delegate_fail`, so no jump can skip its initialiser. Empty means
    * the turn runs in the caller's own cwd. See "ONE ROOT PER DELEGATE TURN". */
   char turn_root[MAX_PATH_LEN] = "";
   cJSON *req = cctx->req;
   bind_request_session_creds(req);
   cJSON *jrole = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   cJSON *jmax = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   cJSON *jmaxcost = cJSON_GetObjectItemCaseSensitive(req, "max_cost_usd");
   cJSON *jsystem = cJSON_GetObjectItemCaseSensitive(req, "system_prompt");
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "delegation_id");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(req, "branch");
   cJSON *jtimeout = cJSON_GetObjectItemCaseSensitive(req, "timeout_ms");
   cJSON *jloop_timeout_cap = cJSON_GetObjectItemCaseSensitive(req, "tool_loop_timeout_ms_cap");
   cJSON *jmaxturns = cJSON_GetObjectItemCaseSensitive(req, "max_turns");
   cJSON *jmaxturnscap = cJSON_GetObjectItemCaseSensitive(req, "max_turns_cap");
   cJSON *jhandoff = cJSON_GetObjectItemCaseSensitive(req, "handoff_json");
   cJSON *jtools = cJSON_GetObjectItemCaseSensitive(req, "tools");
   cJSON *jprovided_target = cJSON_GetObjectItemCaseSensitive(req, "provided_target");
   cJSON *jtier = cJSON_GetObjectItemCaseSensitive(req, "tier");
   cJSON *jvia = cJSON_GetObjectItemCaseSensitive(req, "via");
   cJSON *jparticipant = cJSON_GetObjectItemCaseSensitive(req, "participant");
   cJSON *jprovider = cJSON_GetObjectItemCaseSensitive(req, "provider");
   cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(req, "model");
   cJSON *jparent_deleg = cJSON_GetObjectItemCaseSensitive(req, "parent_delegation_id");
   cJSON *jreq_caps = cJSON_GetObjectItemCaseSensitive(req, "required_caps");
   cJSON *jmin_ctx = cJSON_GetObjectItemCaseSensitive(req, "min_context");
   cJSON *jscope = cJSON_GetObjectItemCaseSensitive(req, "scope");
   const char *role =
       delegate_role_canonicalize(cJSON_IsString(jrole) ? jrole->valuestring : "execute");
   const char *prompt = cJSON_IsString(jprompt) ? jprompt->valuestring : "";
   int max_tokens = cJSON_IsNumber(jmax) ? (int)jmax->valuedouble : 0; /* 0 => model-derived */
   double max_cost_usd = cJSON_IsNumber(jmaxcost) ? jmaxcost->valuedouble : 0.0;
   const char *system_prompt = cJSON_IsString(jsystem) ? jsystem->valuestring : NULL;
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : NULL;
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";
   const char *branch =
       (cJSON_IsString(jbranch) && jbranch->valuestring[0]) ? jbranch->valuestring : NULL;
   int timeout_ms = cJSON_IsNumber(jtimeout) ? (int)jtimeout->valuedouble : 0;
   int tool_loop_timeout_ms_cap =
       cJSON_IsNumber(jloop_timeout_cap) && jloop_timeout_cap->valuedouble > 0
           ? (jloop_timeout_cap->valuedouble > INT_MAX ? INT_MAX
                                                       : (int)jloop_timeout_cap->valuedouble)
           : 0;
   int max_turns = cJSON_IsNumber(jmaxturns) ? (int)jmaxturns->valuedouble : -1;
   int max_turns_cap = cJSON_IsNumber(jmaxturnscap) && jmaxturnscap->valuedouble > 0
                           ? (int)jmaxturnscap->valuedouble
                           : 0;
   int handoff_json = cJSON_IsTrue(jhandoff);
   /* Only the JSON boolean literal true opts out; absent, false, and malformed
    * values preserve automatic evidence grounding. */
   int caller_provided_target = cJSON_IsTrue(jprovided_target);
   const char *toolset_override =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "toolset"));
   int tier_override = cJSON_IsNumber(jtier) ? (int)jtier->valuedouble : -1;
   const char *via_name = cJSON_IsString(jvia) ? jvia->valuestring : NULL;
   const char *participant = cJSON_IsString(jparticipant) ? jparticipant->valuestring : NULL;
   cJSON *jacp_cmd = cJSON_GetObjectItemCaseSensitive(req, "acp_command");
   cJSON *jacp_args = cJSON_GetObjectItemCaseSensitive(req, "acp_args");
   const char *acp_command =
       (cJSON_IsString(jacp_cmd) && jacp_cmd->valuestring[0]) ? jacp_cmd->valuestring : NULL;
   const char *acp_args = cJSON_IsString(jacp_args) ? jacp_args->valuestring : NULL;
   const char *provider_override = cJSON_IsString(jprovider) ? jprovider->valuestring : NULL;
   const char *model_override = cJSON_IsString(jmodel) ? jmodel->valuestring : NULL;
   /* delegate_routing bandit: sampled at the route step below (gated, best-effort)
    * and rewarded with the run outcome at exit. Empty unless a decision was made. */
   char dr_decision_id[KB_BANDIT_MAX_DECISION] = {0};
   char dr_arm_id[KB_BANDIT_MAX_ARM_ID] = {0};
   cJSON *jpersona = cJSON_GetObjectItemCaseSensitive(req, "persona");
   const char *persona_override = cJSON_IsString(jpersona) ? jpersona->valuestring : NULL;
   /* A persona is REQUIRED on every delegate request — it sets the delegate's
    * identity and principles. Every builder that reaches this worker passes one
    * (the MCP delegate tool validates it; coord-task dispatch defaults it); a
    * request without one is a programming error, so reject it rather than
    * silently falling back. */
   if (!persona_override || !persona_override[0])
   {
      delegation_compute_error(cctx, "delegate requires a 'persona' (e.g. engineer, qa, security, "
                                     "reviewer, architect)");
      compute_ctx_free(cctx);
      return;
   }
   /* The role must name a real role, and the persona a real persona. Both used to
    * be taken on trust here, so `delegate bogusrole` and `--persona nosuchpersona`
    * ran to completion: the unknown role fell back to a generic prompt with
    * read-only tool defaults, and the missing persona file left the delegate with
    * no identity or principles at all — in both cases returning a plausible answer
    * to a caller who believed its request had been honoured. The CLI guards these
    * locally, but the /v1 route reaches this worker directly and bypassed it, so
    * the check belongs here, at the single boundary every builder crosses. */
   {
      const char *removed = delegate_role_removed_reason(role);
      if (removed)
      {
         delegation_compute_error(cctx, removed);
         compute_ctx_free(cctx);
         return;
      }
   }
   if (!delegate_role_known(cwd && cwd[0] ? cwd : NULL, role))
   {
      char rolemsg[192];
      snprintf(rolemsg, sizeof(rolemsg),
               "unknown delegate role '%s' (see 'aimee delegate --list-roles')", role);
      delegation_compute_error(cctx, rolemsg);
      compute_ctx_free(cctx);
      return;
   }
   if (!persona_exists(cwd && cwd[0] ? cwd : NULL, persona_override))
   {
      char personamsg[192];
      snprintf(personamsg, sizeof(personamsg), "unknown persona '%s' (see 'aimee persona list')",
               persona_override);
      delegation_compute_error(cctx, personamsg);
      compute_ctx_free(cctx);
      return;
   }
   /* What this delegate may do, resolved ONCE, here, because this is the first
    * point at which the role is known to be real. Everything downstream reads
    * this: the tool default below, the mount, the drift check, the no-op check.
    *
    * It is resolved before the tool default rather than after because that
    * default used to answer from the built-in table while the mount answered
    * from the resolved set -- so a role an operator defined without `tools` was
    * handed them here and refused at dispatch. Same fact, two sources, and the
    * one that ran first was the one that could not see the definition.
    *
    * Failure holds nothing: a delegate whose permissions cannot be established
    * reads and changes nothing. */
   delegate_permissions_t delegate_perms;
   {
      char *role_definition = role_template_frontmatter(cwd[0] ? cwd : NULL, role);
      int perms_rc = delegate_permissions_for_role(role, role_definition, &delegate_perms);
      free(role_definition);
      if (perms_rc != 0)
      {
         char permsmsg[256];
         snprintf(permsmsg, sizeof(permsmsg),
                  "refusing to run delegate: the permissions for role '%s' could not be "
                  "resolved, so it holds none. Check the role template's `permissions:` block.",
                  role);
         delegation_compute_error(cctx, permsmsg);
         compute_ctx_free(cctx);
         return;
      }
   }

   int explicit_tools = cJSON_IsTrue(jtools),
       force_tools = delegate_auto_tools_for_invocation(
           delegate_permissions_has(&delegate_perms, AIMEE_DELEGATES_PERM_TOOLS), max_turns,
           explicit_tools);
   force_tools = force_tools || (toolset_override && toolset_override[0]);
   /* An explicit `tools:false` (CLI --no-tools) overrides the role's tools-on
    * default: an artifact-provided panel review of an inline diff must run
    * tools-off so weaker models don't burn their turns reading files and return
    * nothing. Honored even for tools-on-by-default roles like `review`. */
   if (cJSON_IsBool(jtools) && !cJSON_IsTrue(jtools))
      force_tools = 0;
   /* Generate delegation ID if not provided */
   if (cJSON_IsString(jid) && jid->valuestring[0])
      snprintf(deleg_id, sizeof(deleg_id), "%s", jid->valuestring);
   else
      delegate_generate_id(deleg_id, sizeof(deleg_id));
   /* Publish this slot's identity so `aimee workers` shows the running delegate. */
   compute_pool_set_job(POOL_JOB_DELEGATE, "role=%s sess=%s id=%s", role, sid && sid[0] ? sid : "?",
                        deleg_id);
   /* Validate the request and enforce the persona's delegate policy (none /
    * readonly), resolved from the session's persona or the durable default. */
   char polbuf[192];
   const char *polmsg = server_http_delegate_block(sid, role, prompt, polbuf, sizeof(polbuf));
   if (polmsg)
   {
      delegation_compute_error(cctx, polmsg);
      compute_ctx_free(cctx);
      return;
   }

   /* Load agent config */
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      delegation_compute_error(cctx, "failed to load agent config");
      compute_ctx_free(cctx);
      return;
   }
   if (tool_loop_timeout_ms_cap > 0)
   {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
      {
         acfg.tool_loop_timeout_ms_cap = tool_loop_timeout_ms_cap;
         acfg.tool_loop_deadline_ms =
             (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + tool_loop_timeout_ms_cap;
      }
   }
   /* A participant is a delegate-service continuation token. Resolve it here,
    * behind the generic delegation boundary, so coordinators never learn or
    * retain the concrete agent identity. The durable job row survives service
    * restarts and therefore makes discussion continuity restart-safe. */
   char participant_agent[MAX_AGENT_NAME] = "";
   if (participant && participant[0])
   {
      if (via_name && via_name[0])
      {
         delegation_compute_error(cctx, "invalid delegate participant continuation");
         compute_ctx_free(cctx);
         return;
      }
      db1_agent_job_t prior;
      memset(&prior, 0, sizeof(prior));
      if (db1_agent_job_get_by_participant(participant, &prior) != 0 || !prior.agent_name[0])
      {
         db1_agent_job_free(&prior);
         delegation_compute_error(cctx, "delegate participant continuation is unknown");
         compute_ctx_free(cctx);
         return;
      }
      snprintf(participant_agent, sizeof(participant_agent), "%s", prior.agent_name);
      db1_agent_job_free(&prior);
      via_name = participant_agent;
   }
   /* Inline --acp <cmd>: synthesize an ephemeral kind:acp agent and route to it
    * by name, exactly like --via. The external ACP agent runs its own model, so
    * aimee's capability inference does not apply (caps/context are external). */
   char inline_acp_name[MAX_AGENT_NAME] = "";
   if (acp_command)
   {
      if (delegate_add_inline_acp_agent(&acfg, acp_command, acp_args, role, inline_acp_name,
                                        sizeof(inline_acp_name)) != 0)
      {
         delegation_compute_error(cctx, "failed to register inline --acp delegate agent");
         compute_ctx_free(cctx);
         return;
      }
      via_name = inline_acp_name;
   }

   /* Claude over the standard `claude` CLI/tmux on a DETACHED (thin-client)
    * workspace: claude runs its own interactive session on the CLIENT (the tmux
    * driver marshals its commands over the reverse channel) against the client's
    * tree, doing its own edits — so it bypasses the server-side worktree
    * isolation / write-guard machinery below (which assumes a local fs), exactly
    * as the primary chat path does. Gated by the agent's `primary_only` flag. */
   if (via_name && via_name[0])
   {
      agent_t *cag = agent_find(&acfg, via_name);
      if (cag && agent_is_claude_cli(cag))
      {
         int detached_bound = workspace_turn_bind_active(cwd);
         const workspace_provider_t *wsp = workspace_provider_active();
         if (detached_bound && wsp && wsp->kind == WS_PROVIDER_DETACHED && wsp->exec_shell)
         {
            if (cag->primary_only)
            {
               workspace_turn_unbind_active();
               char m[320];
               snprintf(m, sizeof(m),
                        "agent '%s' is marked Primary Agent Only and cannot run as a delegate; "
                        "uncheck 'Primary Agent Only' for it in the Agents tab to allow delegation "
                        "(see DELEGATES.md for the Anthropic account-risk warning)",
                        cag->name);
               delegation_compute_error(cctx, m);
               compute_ctx_free(cctx);
               return;
            }
            if (cwd[0] == '/' && !strstr(cwd, "/.."))
               run_cmd_set_cwd(cwd);
            char *tmpl = NULL;
            const char *sysp = delegate_assemble_system_prompt(system_prompt, role, prompt, cwd,
                                                               persona_override, cwd, &tmpl);
            agent_result_t result;
            memset(&result, 0, sizeof(result));
            int cli_max_tokens = AGENT_DEFAULT_MAX_TOKENS;
            int cli_cost_failed = 0;
            if (max_cost_usd != 0.0)
            {
               size_t bytes = strlen(sysp ? sysp : "") + strlen(prompt);
               int input_tokens = bytes > (size_t)(INT_MAX - 32768) ? INT_MAX : (int)bytes + 32768;
               char cost_err[256] = "";
               if (max_cost_usd < 0.0 ||
                   delegate_apply_cost_limit(cag, max_cost_usd, input_tokens, &cli_max_tokens,
                                             cost_err, sizeof(cost_err)) != 0)
               {
                  snprintf(result.error, sizeof(result.error), "%s",
                           max_cost_usd < 0.0 ? "max_cost_usd cannot be negative" : cost_err);
                  cli_cost_failed = 1;
               }
            }
            int rc = cli_cost_failed
                         ? -1
                         : agent_execute_cli_session(cag, NULL, sysp ? sysp : "", prompt,
                                                     cli_max_tokens, 0.3, &result);
            run_cmd_set_cwd(NULL);
            workspace_turn_unbind_active();
            free(tmpl);
            cJSON *resp = delegate_build_result_response(deleg_id, rc, &result, &acfg, role, cag,
                                                         -1, 0, NULL, NULL);
            free(result.response);
            compute_respond(cctx, resp);
            compute_ctx_free(cctx);
            return;
         }
         workspace_turn_unbind_active();
      }
   }
   unsigned required_caps = cJSON_IsNumber(jreq_caps) ? (unsigned)jreq_caps->valuedouble : 0;
   int min_context = cJSON_IsNumber(jmin_ctx) ? (int)jmin_ctx->valuedouble : 0;
   /* The packet's scope CEILING. Routing is the server's job, so the ceiling is
    * enforced here rather than by the caller. Absent or unparseable leaves it
    * UNSET, which admits every seat - the client validates the spelling, so an
    * unparseable value here means a non-CLI caller, and the permissive default
    * matches the previous behaviour for every existing caller. */
   agent_scope_t scope =
       cJSON_IsString(jscope) ? agent_scope_from_string(jscope->valuestring) : AGENT_SCOPE_UNSET;
   {
      char route_err[256];
      unsigned inferred_caps = 0;
      int inferred_min_context = 0;
      int drop_deprecated = !(via_name && via_name[0]) &&
                            !(provider_override && provider_override[0]) &&
                            !(model_override && model_override[0]);
      if (acp_command)
      {
         /* External ACP agent: its capabilities are out of aimee's registry, so
          * skip inference and route purely by the synthesized agent's name. */
         required_caps = 0;
         min_context = 0;
      }
      else
      {
         delegate_infer_capability_requirements(prompt, force_tools, &inferred_caps,
                                                &inferred_min_context);
         required_caps |= inferred_caps;
         if (inferred_min_context > min_context)
            min_context = inferred_min_context;
      }
      /* delegate_routing bandit: when the caller gave no explicit route override,
       * sample a cost-tier preference (cheapest vs premium) from the kb DB2 bandit
       * and translate it into tier_override. Gated by bandit_live_decision_enabled
       * (default off); best-effort, so a kb hiccup falls back to default routing. */
      if (!acp_command && tier_override < 0 && !(via_name && via_name[0]) &&
          !(provider_override && provider_override[0]) && !(model_override && model_override[0]))
      {
         if (config_bandit_live_decision_enabled())
         {
            static const char *const dr_arms[2] = {"cheapest", "premium"};
            if (kb_client_bandit_sample("delegate_routing", dr_arms, 2, dr_arm_id,
                                        sizeof(dr_arm_id), dr_decision_id,
                                        sizeof(dr_decision_id)) == 0)
            {
               if (strcmp(dr_arm_id, "premium") == 0)
               {
                  int max_tier = delegate_max_cost_tier(&acfg, role);
                  if (max_tier >= 0)
                     tier_override = max_tier;
               }
               /* "cheapest" leaves tier_override = -1 (default cheapest routing). */
            }
         }
      }
      if (delegate_apply_route_overrides(&acfg, role, via_name, tier_override, provider_override,
                                         model_override, route_err, sizeof(route_err)) != 0 ||
          delegate_filter_route_capabilities(&acfg, role, required_caps, min_context,
                                             drop_deprecated, route_err, sizeof(route_err)) != 0 ||
          delegate_route_preflight(&acfg, role, route_err, sizeof(route_err)) != 0)
      {
         delegation_compute_error(cctx, route_err);
         compute_ctx_free(cctx);
         return;
      }
   }

   agent_route_policy_t policy;
   agent_route_policy_current(&policy);
   target_agent =
       agent_route_with_caps_scoped(&acfg, role, &policy, required_caps, min_context, scope);
   if (!target_agent)
   {
      char caps_buf[128];
      model_capability_format_flags(required_caps, caps_buf, sizeof(caps_buf));
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg),
               "no configured model supports required capabilities (caps=%s, min_context=%d, "
               "scope=%s)",
               caps_buf[0] ? caps_buf : "none", min_context, agent_scope_name(scope));
      delegation_compute_error(cctx, errmsg);
      compute_ctx_free(cctx);
      return;
   }
   /* Record the placement together with the ceiling it was made under. A scope
    * that silently failed to bind is indistinguishable from one that bound and
    * admitted the seat unless the effective value is observable, and this is the
    * only place that knows both. */
   if (scope != AGENT_SCOPE_UNSET)
      aimee_log(LOG_INFO, "delegate", "routed role '%s' to '%s' under scope ceiling %s", role,
                target_agent->name, agent_scope_name(scope));
   /* An agent flagged "Primary Agent Only" (agents.json `primary_only`) is never
    * a delegation target. A claude-oauth subscription is pre-flagged this way at
    * add time: driving a personal Claude plan as an automated delegate may breach
    * Anthropic's terms. (Concise here — the risk warning is in DELEGATES.md.) */
   if (target_agent->primary_only)
   {
      char errmsg[320];
      snprintf(errmsg, sizeof(errmsg),
               "agent '%s' is marked Primary Agent Only and cannot run as a delegate; "
               "uncheck 'Primary Agent Only' for it in the Agents tab to allow delegation "
               "(see DELEGATES.md for the Anthropic account-risk warning)",
               target_agent->name);
      delegation_compute_error(cctx, errmsg);
      compute_ctx_free(cctx);
      return;
   }
   /* Never let a delegate run with a non-positive timeout: timeout_ms <= 0
    * disables the HTTP read deadline and a stalled provider hangs the worker
    * forever, leaking its pool thread + concurrency slot (see
    * delegate_effective_timeout_ms). Resolve request > agent-config > default. */
   if (target_agent)
   {
      target_agent->timeout_ms =
          delegate_effective_timeout_ms(timeout_ms, target_agent->timeout_ms);
      target_agent->tool_loop_timeout_ms_cap = tool_loop_timeout_ms_cap;
   }
   delegate_apply_max_turns_policy(&acfg, role, max_turns);
   delegate_apply_max_turns_cap(&acfg, role, max_turns_cap);
   if (cctx->background_job_id > 0 && target_agent)
      db1_agent_job_set_agent(cctx->background_job_id, target_agent->name);
   /* Resolve the credential (WP-C.1 vault-FIRST + WP-C.3 per-principal cooldown):
    * sets target_agent->api_key on a hit and names the pool row to release on
    * exit; the worker maps a non-OK status to a delegate error. */
   int cooldown_secs = 0;
   delegate_cred_resolve_status_t cred_status = delegate_resolve_credentials(
       target_agent ? cctx->vault_principal : NULL, target_agent, leased_principal,
       sizeof(leased_principal), leased_cred_name, sizeof(leased_cred_name), credential_state_path,
       sizeof(credential_state_path), &cooldown_secs);
   if (cred_status != DELEGATE_CRED_RESOLVE_OK)
   {
      char errmsg[200];
      if (cred_status == DELEGATE_CRED_RESOLVE_LOCKED)
         snprintf(errmsg, sizeof(errmsg), "vault locked: run `aimee vault unlock`");
      else if (cred_status == DELEGATE_CRED_RESOLVE_COOLING)
         snprintf(errmsg, sizeof(errmsg),
                  "vault credential for agent '%s' is rate-limited; retry in %ds",
                  target_agent->name, cooldown_secs);
      else
         snprintf(errmsg, sizeof(errmsg), "no available credential in pool for agent '%s'",
                  target_agent->name);
      delegation_compute_error(cctx, errmsg);
      compute_ctx_free(cctx);
      return;
   }

   /* Enforce delegation depth limit.
    *
    * For in-process sub-delegations (agent using mcp__aimee__delegate), the
    * thread-local tl_delegation_depth is authoritative. For cross-process
    * chains (agent shells out to aimee-client), the client sends
    * AIMEE_DELEGATE_DEPTH/AIMEE_PARENT_DELEGATION_ID as request fields. Stale
    * completed parents are ignored so a primary shell with leaked env does not
    * get misclassified as a live delegate. */
   int max_depth = config_max_delegation_depth() > 0 ? config_max_delegation_depth()
                                                     : CONFIG_DEFAULT_MAX_DELEGATION_DEPTH;
   cJSON *jreq_depth = cJSON_GetObjectItemCaseSensitive(req, "delegation_depth");
   int req_parent_depth = 0;
   const char *request_parent = NULL;
   delegate_request_parent_context(jreq_depth, jparent_deleg, &req_parent_depth, &request_parent);
   int parent_depth =
       tl_delegation_depth > req_parent_depth ? tl_delegation_depth : req_parent_depth;
   int current_depth = parent_depth + 1;
   if (current_depth > max_depth)
   {
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg),
               "delegation depth limit exceeded (%d/%d). "
               "Reduce nesting or increase max_delegation_depth in config.",
               current_depth, max_depth);
      delegation_compute_error(cctx, errmsg);
      goto delegate_fail;
   }

   /* Determine effective parent ID: in-process thread-local takes priority;
    * fall back to the request field for cross-process sub-delegations where
    * the child aimee-client propagates AIMEE_PARENT_DELEGATION_ID. */
   const char *effective_parent =
       tl_parent_delegation_id[0] ? tl_parent_delegation_id : request_parent;

   /* Enforce delegation spawn limit for nested descendants of one top-level
    * delegate. Counting all first-level delegates for the whole operator
    * session exhausts long, deliberate delegate-heavy workflows; runaway risk
    * comes from sub-delegation fan-out below a root delegate. */
   int max_spawns = config_max_delegation_spawns() > 0 ? config_max_delegation_spawns()
                                                       : CONFIG_DEFAULT_MAX_DELEGATION_SPAWNS;
   const char *effective_sid = sid ? sid : session_id();
   int total_spawns = 0;
   char root_deleg_id[64] = "";
   if (current_depth > 1)
   {
      if (effective_parent && effective_parent[0] &&
          db1_delegation_spawn_find_root(effective_parent, root_deleg_id, sizeof(root_deleg_id)) ==
              0)
         total_spawns = db1_delegation_spawn_count_descendants(root_deleg_id);
      else
         total_spawns = db1_delegation_spawn_count_total(effective_sid);
      if (total_spawns >= max_spawns)
      {
         char errmsg[256];
         snprintf(errmsg, sizeof(errmsg),
                  "delegation spawn limit exceeded (%d/%d nested delegates for root). "
                  "Reduce sub-delegation fan-out or increase max_delegation_spawns.",
                  total_spawns, max_spawns);
         delegation_compute_error(cctx, errmsg);
         goto delegate_fail;
      }
   }

   /* ONE ROOT PER DELEGATE TURN.
    *
    * Everything below derives from `cwd`: the worktree the file tools write to
    * (delegate_resolve_worktree), the working directory the shell runs in
    * (run_cmd_set_cwd), the absolute paths rewritten in the prompt, the tree the
    * no-op detector diffs, the parent-write guard. Resolving the turn's root
    * HERE -- once, before any of them -- is what stops them disagreeing.
    *
    * They used to. A background delegate on a DETACHED (client-served) workspace
    * had its shell redirected into a server-side tree hundreds of lines below
    * this point, after the worktree had already been resolved against the
    * client's path. One delegate, two roots: it wrote through a path that existed
    * only on the client and ran `pwd` somewhere else entirely. So it could edit
    * code and could not build, test, or diff the edit -- and nothing told it so.
    * Measured: asked to add one comment line to a 2157-line file, such a delegate
    * truncated it to 5 lines and reported success.
    *
    * A no-op for every other turn. The resolver returns 0 unless this is a
    * background job on a detached workspace that has recorded a remote and head
    * via `aimee workspace mirror-sync`; a detached workspace with nothing
    * recorded is handled further down, where having no usable tree at all is a
    * refusal rather than a redirect. */
   if (cwd[0] && cctx->background_job_id > 0 &&
       workspace_turn_resolve_detached_mirror_cwd(cwd, turn_root, sizeof(turn_root)) &&
       turn_root[0])
   {
      aimee_log(LOG_WARN, "delegate",
                "delegate %s: detached workspace '%s' has no client to serve this background job; "
                "binding the whole turn -- shell and file tools alike -- to the server-side "
                "reconstruction at %s. That tree is the last state synced by `aimee workspace "
                "mirror-sync` and may be behind the client",
                deleg_id, cwd, turn_root);
      cwd = turn_root;
   }
   else
      turn_root[0] = '\0'; /* a partial write from a failed resolve is not a root */

   /* Resolve @path/to/file references in the delegate prompt */
   if (strchr(prompt, '@'))
   {
      resolved_prompt = resolve_file_references(prompt, cwd[0] ? cwd : ".");
      if (resolved_prompt)
         prompt = resolved_prompt;
   }
   /* Read, not resolved: delegate_perms was established when the role was
    * validated, above. The worktree plan and the container spec both consume
    * this, and it is the one fact they must agree on -- working it out in two
    * places is how a delegate ends up planned read-only and mounted writable.
    *
    * The brief is not consulted. A phrase like "do not edit files" used to
    * narrow a write role to read-only, which meant the same delegate had
    * different powers depending on how its prompt was worded. The role decides;
    * a read-only run is a read-only role. */
   /* Scoped against the repository the CALLER named. That is the object an
    * operator means by `scopes: [/srv/repo-a]`, and it is the only one known
    * this early -- the delegate's own worktree does not exist yet, and its path
    * would not match a scope anyone wrote.
    *
    * Matching is exact, so a delegate pointed at a subdirectory of a scoped
    * repository is read-only. That is the documented rule and the safe one: the
    * alternative is a prefix match, where /srv/repo grants /srv/repo-secrets.
    *
    * No cwd and a scoped grant means read-only too. Nothing shows the target is
    * in scope, and "probably fine" is not a permission. */
   int delegate_allows_writes =
       delegate_permissions_allow(&delegate_perms, AIMEE_DELEGATES_PERM_REPO_WRITE, cwd);
   if (!delegate_allows_writes &&
       delegate_permissions_has(&delegate_perms, AIMEE_DELEGATES_PERM_REPO_WRITE))
      aimee_log(LOG_INFO, "delegate",
                "delegate %s: role '%s' holds repo_write but not for '%s', so it runs read-only",
                deleg_id, role, cwd[0] ? cwd : "(no workspace named)");
   if (branch && !delegate_allows_writes)
   {
      delegation_compute_error(cctx, "read-only delegates must use the parent worktree; branch "
                                     "requests require a sibling delegate worktree");
      goto delegate_fail;
   }
   /* Isolate a write delegate in its own sibling worktree only when it runs
    * concurrently — a background job, a parallel (coord) task, or an explicit
    * branch. A foreground delegate is the sole writer (the parent turn blocks on
    * it), so it shares the parent worktree and works on its live state. */
   int delegate_concurrent = (cctx->background_job_id > 0) || (cctx->coord_task_id > 0);
   int delegate_needs_worktree = delegate_allows_writes && (delegate_concurrent || branch != NULL);

   /* Set thread-local CWD for delegate execution (validate: absolute, no traversal) */
   if (cwd[0] && cwd[0] == '/' && !strstr(cwd, "/../") && !strstr(cwd, "/.."))
      run_cmd_set_cwd(cwd);

   /* Read-only delegates use parent workspace; write-capable delegates use sibling worktrees.
    * (worktree path/git_root/work_name are hoisted for delegate_fail: cleanup.) */
   int delegate_worktree_attempted = 0;
   int delegate_shared_worktree = 0;
   int delegate_dedicated_worktree = 0;
   {
      delegate_worktree_t wt;
      delegate_resolve_worktree(cwd, deleg_id, branch, delegate_allows_writes,
                                delegate_needs_worktree, &wt);
      snprintf(delegate_worktree_path, sizeof(delegate_worktree_path), "%s", wt.worktree_path);
      snprintf(delegate_git_root, sizeof(delegate_git_root), "%s", wt.git_root);
      snprintf(delegate_work_name, sizeof(delegate_work_name), "%s", wt.work_name);
      delegate_worktree_attempted = wt.attempted;
      delegate_shared_worktree = wt.shared;
      delegate_dedicated_worktree = wt.dedicated;
   }

   if (delegate_allows_writes && delegate_worktree_attempted && !delegate_worktree_path[0])
   {
      char errmsg[512];
      snprintf(errmsg, sizeof(errmsg),
               "refusing to run write-capable delegate in parent worktree '%s': "
               "could not create an isolated delegate worktree",
               cwd[0] ? cwd : delegate_git_root);
      delegation_compute_error(cctx, errmsg);
      goto delegate_fail;
   }

   /* Rewrite operator-cwd absolute paths so provider/tool writes stay isolated. */
   if (!delegate_shared_worktree && delegate_worktree_path[0] && cwd[0] == '/' &&
       strcmp(cwd, delegate_worktree_path) != 0)
   {
      int occurrences = 0;
      char *rewritten =
          delegate_rewrite_prompt_cwd(prompt, cwd, delegate_worktree_path, &occurrences);
      if (rewritten)
      {
         free(resolved_prompt);
         resolved_prompt = rewritten;
         prompt = resolved_prompt;
         aimee_log(LOG_INFO, "delegate",
                   "rewrote %d operator-cwd path(s) in prompt to delegate worktree (id=%s)",
                   occurrences, deleg_id);
      }
   }

   char launch_worktree_path[MAX_PATH_LEN] = "", parent_worktree_path[MAX_PATH_LEN] = "";
   snprintf(launch_worktree_path, sizeof(launch_worktree_path), "%s",
            delegate_worktree_path[0] ? delegate_worktree_path : (cwd[0] ? cwd : "."));
   char launch_head[64] = "", parent_worktree_head[64] = "";
   (void)delegate_git_head(launch_worktree_path, launch_head, sizeof(launch_head));
   snprintf(parent_worktree_path, sizeof(parent_worktree_path), "%s",
            cwd[0] ? cwd : launch_worktree_path);
   (void)delegate_git_head(parent_worktree_path, parent_worktree_head,
                           sizeof(parent_worktree_head));
   char parent_worktree_fingerprint[64] = "";
   (void)delegate_git_worktree_fingerprint(parent_worktree_path, parent_worktree_fingerprint,
                                           sizeof(parent_worktree_fingerprint));
   /* Assemble the system prompt: per-role template (or fallback), persona
    * identity/principles, and token-budget shedding. template_sys_prompt owns
    * the buffer (NULL when the static fallback literal is in use). */
   system_prompt =
       delegate_assemble_system_prompt(system_prompt, role, prompt, cwd, persona_override,
                                       delegate_worktree_path, &template_sys_prompt);

   /* This is delegate_worker's sole parent-diff evidence injection point.
    * Ground read-only inspection roles in parent diff evidence unless the
    * caller supplied the complete review target inline. In that case an
    * unrelated current-worktree diff is competing evidence and can make a
    * plan reviewer incorrectly demand implementation. */
   if (!caller_provided_target)
   {
      char *evidence = delegate_prepend_parent_diff_evidence(prompt, role, delegate_allows_writes,
                                                             cwd, deleg_id);
      if (evidence)
      {
         free(resolved_prompt);
         resolved_prompt = evidence;
         prompt = resolved_prompt;
      }
   }

   /* Automatic context injection: query the code index for terms in the prompt
    * and append a ## Context block so the delegate starts with relevant snippets
    * already loaded. Silently skips if kb is unreachable. */
   delegate_append_owned_block(&system_prompt, &template_sys_prompt,
                               delegate_inject_code_context(prompt));

   /* §7 graph-informed delegation (opt-in, fail-open): prepend the structural
    * neighborhood (callers/dependencies) of the files the task references so the
    * delegate sees the blast radius of a shared interface up front. */
   delegate_append_owned_block(&system_prompt, &template_sys_prompt,
                               delegate_inject_graph_context(prompt, cwd));

   /* Named-file drift guard: extract any repo-relative paths named in the prompt
    * and check pre-flight conditions before running the agent. */
   char named_paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int named_path_count =
       delegate_extract_named_paths(prompt, named_paths, DELEGATE_DRIFT_MAX_PATHS);

   /* Warn weaker delegates on multi-file scope; codex can handle whole features. */
   int suppress_scope_warn =
       target_agent && target_agent->cli_kind[0] && strcmp(target_agent->cli_kind, "codex") == 0;
   if (named_path_count > 1 && !suppress_scope_warn)
   {
      char files_list[1024];
      int fl_pos = 0;
      for (int i = 0; i < named_path_count && fl_pos < (int)sizeof(files_list) - 2; i++)
      {
         if (i > 0)
            files_list[fl_pos++] = ' ';
         int w = snprintf(files_list + fl_pos, sizeof(files_list) - (size_t)fl_pos, "%s",
                          named_paths[i]);
         if (w > 0)
            fl_pos += w;
      }
      files_list[fl_pos] = '\0';
      aimee_log(LOG_WARN, "delegate",
                "[delegate-scope-warn] prompt names %d source files (%s) — "
                "delegates should target one file; controlling AI handles cross-file wiring",
                named_path_count, files_list);
   }

   /* Snapshot mtimes and HEAD to detect no-op write delegates. */
   delegate_file_snapshot_t pre_run_files[DELEGATE_DRIFT_MAX_PATHS];
   char pre_run_head_sha[64] = "";
   if (delegate_allows_writes)
   {
      const char *check_root =
          delegate_worktree_path[0] ? delegate_worktree_path : (cwd[0] ? cwd : ".");
      if (named_path_count > 0)
      {
         for (int i = 0; i < named_path_count; i++)
         {
            char full[MAX_PATH_LEN];
            if (named_paths[i][0] == '/')
               snprintf(full, sizeof(full), "%s", named_paths[i]);
            else
               snprintf(full, sizeof(full), "%s/%s", check_root, named_paths[i]);
            pre_run_files[i] = delegate_file_snapshot(full);
         }
      }
      if (check_root && check_root[0])
         (void)delegate_git_head(check_root, pre_run_head_sha, sizeof(pre_run_head_sha));
   }
   if (named_path_count > 0)
   {
      const char *path_ptrs[DELEGATE_DRIFT_MAX_PATHS];
      for (int i = 0; i < named_path_count; i++)
         path_ptrs[i] = named_paths[i];
      char drift_err[512];
      const char *drift_root =
          delegate_worktree_path[0] ? delegate_worktree_path : (cwd[0] ? cwd : NULL);
      int drift_rc =
          delegate_check_named_file_drift(path_ptrs, named_path_count, prompt, NULL, drift_root,
                                          delegate_allows_writes, drift_err, sizeof(drift_err));
      if (drift_rc < 0)
      {
         aimee_log(LOG_WARN, "delegate", "named-file drift guard (pre-flight): %s", drift_err);
         cJSON *eresp = cJSON_CreateObject();
         cJSON_AddStringToObject(eresp, "delegation_id", deleg_id);
         cJSON_AddStringToObject(eresp, "status", "error");
         cJSON_AddStringToObject(eresp, "message", drift_err);
         compute_respond(cctx, eresp);
         goto delegate_fail;
      }
   }

   delegate_run_ctx_t run_ctx;
   delegate_run_ctx_enter(&run_ctx, deleg_id, sid, current_depth,
                          delegate_worktree_path[0] ? delegate_worktree_path : (cwd[0] ? cwd : ""));
   (void)db1_delegation_spawn_record(deleg_id, effective_parent, effective_sid, current_depth,
                                     role);

   /* Concurrency (global + per-agent + per-model caps) is enforced downstream by the single
    * agent_admission controller inside agent_dispatch_one — there is no separate per-model
    * gate here. A pinned turn blocks+queues there; fan-out/fallback fail fast. */
   agent_result_t result;
   memset(&result, 0, sizeof(result));
   char *handoff_prompt = NULL;
   const char *run_prompt = prompt;
   int parent_write_guard_active = 0;
   if (handoff_json)
   {
      handoff_prompt = delegate_handoff_append_contract(prompt, NULL);
      if (handoff_prompt)
         run_prompt = handoff_prompt;
   }
   char *learning_sys_prompt =
       delegate_agent_uses_mistral_path(target_agent)
           ? NULL
           : delegate_learning_inject_prompt(role, system_prompt ? system_prompt : "", 3);
   if (learning_sys_prompt)
      system_prompt = learning_sys_prompt;

   int rc = -1;
   int cost_limit_failed = 0;
   if (max_cost_usd != 0.0)
   {
      size_t input_bytes =
          strlen(run_prompt ? run_prompt : "") + strlen(system_prompt ? system_prompt : "");
      /* One byte per token is a conservative tokenizer-independent ceiling.
       * Reserve additional input for tool schemas/provider framing that is not
       * present in the two prompt strings. */
      const size_t framing_tokens = 32768;
      int input_tokens = input_bytes > (size_t)(INT_MAX - (int)framing_tokens)
                             ? INT_MAX
                             : (int)input_bytes + (int)framing_tokens;
      char cost_err[256] = "";
      if (max_cost_usd < 0.0 ||
          delegate_apply_cost_limit(target_agent, max_cost_usd, input_tokens, &max_tokens, cost_err,
                                    sizeof(cost_err)) != 0)
      {
         snprintf(result.error, sizeof(result.error), "%s",
                  max_cost_usd < 0.0 ? "max_cost_usd cannot be negative" : cost_err);
         rc = -1;
         cost_limit_failed = 1;
      }
   }

   /* Provider-backed delegates can call back into aimee-server tools while
    * waiting on the model. Keep the server compute budget available for
    * those callbacks; delegate concurrency is already governed above by the
    * per-model/provider limiter. */
   compute_ctx_release_budget(cctx);

   /* Single-sourced read-only gate: a non-write-capable delegate is refused ALL
    * native-tool file writes, mirroring the codex read-only sandbox. Set
    * UNCONDITIONALLY (before the cwd-dependent parent-write guard) so a pooled
    * worker thread never inherits a prior delegate's capability. */
   agent_tools_write_capable_set(delegate_allows_writes);
   if (cwd[0])
   {
      const char *write_root = delegate_worktree_path[0] ? delegate_worktree_path : NULL;
      agent_tools_parent_write_guard_set(cwd, write_root);
      parent_write_guard_active = 1;
   }

   /* Thread-local, not the process env: delegate turns run on POOLED worker threads
    * and overlap by design (session_threads defaults above 1; panels fan out through
    * agent_run_parallel). The old save/restore around a process-wide setenv looked
    * scoped but was not — a concurrent delegate's value decided what this one could
    * call, so a reviewer could resolve a coder's toolset. That is the boundary
    * agent_tools_filter_for_role exists to enforce. */
   agent_tools_set_active_toolset(toolset_override);
   /* Bind detached workspace: delegate reads the client's live files (no-op if shared).
    *
    * Skipped when the turn was already moved to a server-side reconstruction
    * above: there is no client workspace left to bind, and the whole turn --
    * cwd, worktree, shell -- is server-side by then. */
   int detached_bound = (cwd[0] && !turn_root[0]) ? workspace_turn_bind_active(cwd) : 0;
   /* A background/durable delegate has no live client connection to serve a
    * DETACHED (client-served) workspace: by the time the worker runs, the
    * dispatching client has disconnected, so the reverse channel is dead and every
    * shell/file tool marshalled to it fails (previously a silent exit_code:-1).
    * Create a server-side ephemeral workspace FIRST; only on success unbind the
    * detached provider and run tools there. On failure keep the detached binding
    * so the dead-channel path surfaces a clear error (fail CLOSED — never run in
    * an undefined cwd). The ephemeral workspace does NOT contain the client's repo
    * (it drops an AIMEE_WORKSPACE_NOTE.txt saying so); a background *code* delegate
    * that must edit the client tree needs it provisioned server-side (follow-up). */
   /* The mirror reconstruction is resolved far above, where rebinding the root
    * still moves the whole turn. Reaching here means there was none: a detached
    * workspace with no recorded remote/head, i.e. one that has never been synced.
    * The only tree left to offer is a repo-less scratch dir. */
   char ephemeral_ws[MAX_PATH_LEN] = "";
   if (detached_bound && cctx->background_job_id > 0)
   {
      if (delegate_ephemeral_ws_create(deleg_id, ephemeral_ws, sizeof(ephemeral_ws)) == 0 &&
          ephemeral_ws[0])
      {
         /* The ephemeral workspace holds no repository, so a WRITE delegate
          * redirected here can still edit the tree through its file tools (they
          * resolve the registered workspace, not this cwd) while every shell
          * command runs somewhere that has no checkout. It can change code and
          * cannot build, test, or diff what it changed -- and nothing tells it so.
          * Observed: a delegate asked to add one comment line to a 2157-line file
          * truncated it to 5 lines and reported no error.
          *
          * Writing without any means of verification is not a degraded mode, it is
          * an unsafe one, so refuse the dispatch and say why. Read-only delegates
          * are unaffected: inspection in a repo-less cwd is merely useless, not
          * destructive, and their file tools still reach the real workspace. */
         if (delegate_allows_writes)
         {
            char errmsg[1024];
            snprintf(errmsg, sizeof(errmsg),
                     "refusing to run write-capable delegate %s: its detached (client) workspace "
                     "cannot be served by a background job, so its shell would run in ephemeral "
                     "workspace '%s', which contains no checkout. The delegate could edit the "
                     "repository through its file tools but could not build or test the result. "
                     "A detached workspace is served by its client, so a background job cannot "
                     "reach it at all. Either keep the client serving it -- run this delegate in "
                     "the foreground with `aimee workspace serve` -- or register the repository "
                     "as a mirror workspace (`aimee workspace add <path> --provider mirror "
                     "--remote <url>`), which the server reconstructs from its own bare mirror at "
                     "the recorded head and can therefore serve with no client present.",
                     deleg_id, ephemeral_ws);
            aimee_log(LOG_ERROR, "delegate", "%s", errmsg);
            delegate_ephemeral_ws_remove(ephemeral_ws);
            ephemeral_ws[0] = '\0';
            delegation_compute_error(cctx, errmsg);
            goto delegate_fail;
         }
         workspace_turn_unbind_active();
         detached_bound = 0;
         run_cmd_set_cwd(ephemeral_ws);
         aimee_log(LOG_WARN, "delegate",
                   "delegate %s: background job cannot serve its detached (client) workspace; "
                   "running tools in server-side ephemeral workspace %s",
                   deleg_id, ephemeral_ws);
      }
      else
      {
         /* Fail closed: detached binding stays, so shell tools return the clear
          * reverse-channel-unavailable error rather than running in a stray cwd. */
         aimee_log(LOG_ERROR, "delegate",
                   "delegate %s: background job on a detached workspace but could not create a "
                   "server-side ephemeral workspace; shell tools will report the reverse channel "
                   "is unavailable",
                   deleg_id);
      }
   }

   /* Every root decision for this turn is now final, so tell the delegate where
    * it is standing and what kind of place that is. Placed HERE and not with the
    * other prompt blocks above: up there the ephemeral fallback had not run yet,
    * and a notice that names the wrong root is worse than none. The delegate is
    * the one that has to decide whether its evidence means what it appears to
    * mean, and it cannot do that while having to infer its own location from
    * whether commands happen to succeed. */
   {
      const char *shell_root = run_cmd_get_cwd();
      const char *file_root =
          delegate_worktree_path[0] ? delegate_worktree_path : (cwd[0] ? cwd : NULL);
      delegate_root_kind_t root_kind = ephemeral_ws[0] ? DELEGATE_ROOT_EPHEMERAL
                                       : turn_root[0]  ? DELEGATE_ROOT_RECONSTRUCTED
                                                       : DELEGATE_ROOT_NAMED;
      aimee_log(LOG_INFO, "delegate", "delegate %s: bound root %s (%s)%s%s", deleg_id,
                shell_root ? shell_root : (file_root ? file_root : "(none)"),
                root_kind == DELEGATE_ROOT_EPHEMERAL       ? "ephemeral workspace, no repository"
                : root_kind == DELEGATE_ROOT_RECONSTRUCTED ? "server-side reconstruction"
                                                           : "caller's workspace",
                (shell_root && file_root && strcmp(shell_root, file_root) != 0)
                    ? "; DIVERGED from file-tool root "
                    : "",
                (shell_root && file_root && strcmp(shell_root, file_root) != 0) ? file_root : "");
      delegate_append_owned_block(&system_prompt, &template_sys_prompt,
                                  delegate_bound_root_notice(shell_root, file_root, root_kind));
   }

   /* Delegate sandbox (default OFF): run this delegate's shell and file ops INSIDE
    * its own container rather than in-process here.
    *
    * Mutually exclusive with the detached binding above, and that is not a policy
    * choice — a DETACHED workspace's files live on the CLIENT, served over the
    * reverse channel, so a server-side container cannot see them. Binding both
    * would silently replace the client's tree with an unrelated empty one. So the
    * sandbox only applies where the files are already server-side: a shared
    * workspace, or the ephemeral fallback above (whose client has disconnected).
    *
    * Bound AFTER the write guards and the detached/ephemeral resolution: those
    * decide policy and WHICH tree, identical for every provider; this only changes
    * WHERE the already-resolved I/O runs. Off — or if no container can be acquired
    * — it returns 0 and the turn runs in-process exactly as today; the failure
    * paths log at ERROR rather than falling back silently.
    *
    * Keyed by deleg_id, so each delegation gets its own container. */
   /* Mount the tree the delegate already has server-side, so the container gets
    * the ENTIRE CURRENT SOURCE TREE — by bind-mount, so it IS that tree rather
    * than a copy that can drift from it. Without it the backend mints an empty
    * scratch dir, and the delegate opens the file named in its task to find
    * nothing.
    *
    * A DELEGATE'S CHANGES MUST NOT LEAVE ITS CONTAINER. That is what decides the
    * mode here, and aimee already draws the line this needs: write-capable
    * delegates get their OWN sibling worktree, read-only delegates share the
    * parent workspace (delegate_resolve_worktree). So:
    *
    *   its own worktree  -> read-write. Isolated by construction: nothing else is
    *                        looking at that tree, and git shares the object store
    *                        rather than copying it.
    *   the shared parent -> READ-ONLY. Not because the delegate is untrusted, but
    *                        because the tree is not its own; two delegates on one
    *                        tree would write over each other with no way to tell.
    *                        Read-only at the mount is a property, not a rule the
    *                        way the write guard above is.
    *
    * The write guard still applies above this; the two agree, and neither is load-
    * bearing alone. */
   const char *container_ws = NULL;
   int container_ws_ro = 1;
   if (delegate_worktree_path[0] && (delegate_dedicated_worktree || !delegate_shared_worktree))
   {
      /* This delegate's own tree — a sibling worktree, or a WFE per-slice tree it
       * owns exclusively (dedicated). Either way nothing else looks at it, so mount
       * it read-WRITE: the delegate's edits land in the tree the engine then diffs
       * and commits. */
      container_ws = delegate_worktree_path;
      container_ws_ro = 0;
   }
   else if (cwd[0] == '/' && !delegate_allows_writes)
   {
      container_ws = cwd; /* shared: readable, never writable */
      container_ws_ro = 1;
   }
   /* A WRITE-capable delegate with no tree of its own has nothing safe to mount: a
    * shared tree must stay read-only, and a writer cannot use a read-only tree. It
    * used to be run unsandboxed on the host under the write guard, which was a
    * second execution model standing beside the container one. There is only the
    * container now, so this refuses: container_ws stays NULL and the bind below
    * turns that into a hard failure rather than a host run. */
   else if (cwd[0] == '/' && delegate_allows_writes)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "delegate %s: write-capable but has no worktree of its own (shared=%d) — refusing "
                "to run. A write delegate needs its own worktree to be given a container",
                deleg_id, delegate_shared_worktree);
   }
   /* Resolve the per-project/-workspace/-global sandbox image (pre-baked `image:`
    * form); NULL falls back to the backend default. Keyed on the mounted worktree,
    * which is under the delegate's repo/workspace. */
   char sbx_image[256];
   const char *sbx_image_arg =
       (container_ws &&
        delegate_sandbox_resolve_image(container_ws, sbx_image, sizeof(sbx_image)) == 0)
           ? sbx_image
           : NULL;
   /* container_ws == NULL has two causes, and they end differently.
    *
    * For a WRITE delegate it is the no-worktree-of-its-own case resolved above, and
    * it REFUSES. It must never become a bind with a NULL workspace: that mints an
    * EMPTY scratch tree and mounts THAT, so the delegate edits files the engine
    * never sees ("write role reported success but produced no diff"). A WFE
    * implement slice is NOT this case: its cwd already IS a dedicated per-slice
    * worktree it owns, so delegate_resolve_worktree marks it dedicated and
    * container_ws points at that tree read-write (above).
    *
    * For a READ-ONLY delegate it means there is no repository in play at all. That
    * still gets a container — the backend's scratch dir — because the mount is a
    * parameter of the single container path, not a second path. An empty tree is
    * harmless to a delegate that was never going to write one.
    *
    * A DETACHED workspace is still exempt. It is served by the connected client
    * over the reverse channel, not by this host, so there is no local tree to put
    * in a container. That is a separate execution model from the in-process host
    * path this change removed, and collapsing it is not this change's business. */
   int container_bound =
       detached_bound ? 0
       : (!container_ws || !container_ws[0]) && delegate_allows_writes
           ? -1
           : workspace_turn_bind_container(deleg_id, sbx_image_arg, container_ws, container_ws_ro);

   if (cost_limit_failed)
   {
      /* Result error was populated before any provider dispatch. */
   }
   else if (container_bound < 0)
   {
      /* The delegate could not be given a container: no worktree of its own to
       * mount, no docker backend, or a runtime that would not isolate it. There is
       * no un-sandboxed path to fall back to, so the delegation fails. The specific
       * cause was logged where it was detected. */
      memset(&result, 0, sizeof(result));
      snprintf(result.error, sizeof(result.error),
               "delegate could not be given a sandboxed container; refusing to run it "
               "un-sandboxed (see the delegate-sandbox log for the cause)");
      rc = -1;
   }
   else
   {
      /* Inside a container the mount IS the isolation boundary: the delegate's
       * writes go through the provider into its own RW-mounted tree, which nothing
       * else on the host can see. The host-side parent-write guard exists only for
       * the in-process (same-host) path; here it is redundant and would only risk
       * blocking a legitimate write, so drop it once a container is actually bound. */
      if (container_bound > 0)
      {
         agent_tools_parent_write_guard_clear();
         parent_write_guard_active = 0;
      }
      server_delegate_heartbeat_begin(cctx->background_job_id);
      rc = delegate_run_with_credential_retry(&acfg, target_agent, role, system_prompt, run_prompt,
                                              max_tokens, force_tools, delegate_allows_writes,
                                              leased_cred_name, sizeof(leased_cred_name),
                                              credential_state_path, &result);
      server_delegate_heartbeat_end();
   }
   delegate_run_ctx_restore(&run_ctx);
   if (detached_bound) /* unbind last: keep the binding live for any teardown that consults it */
      workspace_turn_unbind_active();
   /* Same call, and never both (see the bind): it clears the active pointer AND
    * releases the container. Only when a container was actually bound (>0); a hard
    * isolation refusal (<0) already tore the container down and set no active binding. */
   if (container_bound > 0)
      workspace_turn_unbind_active();
   if (ephemeral_ws[0])
   {
      /* Clear the thread-local cwd BEFORE removing the workspace so any post-run
       * teardown exec (transcript capture, etc.) does not try to `cd` into a
       * directory we just deleted (a harmless-but-noisy "cd: can't cd" error). */
      run_cmd_set_cwd(NULL);
      delegate_ephemeral_ws_remove(ephemeral_ws);
   }
   (void)db1_delegation_spawn_complete(deleg_id);

   /* Post-run named-file drift check: verify named existing paths appear in response. */
   if (named_path_count > 0 && rc == 0 && result.response && result.response[0])
   {
      const char *path_ptrs[DELEGATE_DRIFT_MAX_PATHS];
      for (int i = 0; i < named_path_count; i++)
         path_ptrs[i] = named_paths[i];
      char drift_err[512];
      int drift_rc =
          delegate_check_named_file_drift(path_ptrs, named_path_count, prompt, result.response,
                                          delegate_worktree_path[0] ? delegate_worktree_path : NULL,
                                          delegate_allows_writes, drift_err, sizeof(drift_err));
      if (drift_rc < 0)
      {
         aimee_log(LOG_WARN, "delegate", "named-file drift (post-run): %s", drift_err);
         rc = -1;
         snprintf(result.error, sizeof(result.error), "%s", drift_err);
      }
      else if (drift_rc > 0)
      {
         aimee_log(LOG_INFO, "delegate", "named-file drift warning: %s", drift_err);
      }
   }

   /* Flag a write delegate that reported success but changed nothing. */
   {
      char noop_err[256] = "";
      if (delegate_detect_noop_write(delegate_allows_writes, handoff_json, rc, named_paths,
                                     named_path_count, pre_run_files, pre_run_head_sha,
                                     delegate_worktree_path, cwd, deleg_id, sid, role, noop_err,
                                     sizeof(noop_err)))
      {
         rc = -1;
         snprintf(result.error, sizeof(result.error), "%s", noop_err);
      }
   }

   delegate_handoff_validation_t handoff_validation;
   memset(&handoff_validation, 0, sizeof(handoff_validation));
   int handoff_checked = 0;
   if (handoff_json && rc == 0)
   {
      handoff_checked = 1;
      (void)delegate_handoff_validate_text(result.response, NULL, 1, &handoff_validation);
      if (!handoff_validation.valid)
      {
         char *repair_prompt =
             delegate_handoff_repair_prompt(result.response, handoff_validation.error);
         if (repair_prompt)
         {
            agent_result_t repaired;
            memset(&repaired, 0, sizeof(repaired));
            int repair_rc =
                force_tools
                    ? agent_run_with_tools_write_enforce(&acfg, role, system_prompt, repair_prompt,
                                                         max_tokens, delegate_allows_writes,
                                                         &repaired)
                    : agent_run(&acfg, role, system_prompt, repair_prompt, max_tokens, &repaired);
            free(repair_prompt);
            if (repair_rc == 0 && repaired.response)
            {
               free(result.response);
               result.response = repaired.response;
               repaired.response = NULL;
               result.turns += repaired.turns;
               result.tool_calls += repaired.tool_calls;
               if (repaired.agent_name[0])
                  snprintf(result.agent_name, sizeof(result.agent_name), "%s", repaired.agent_name);
               (void)delegate_handoff_validate_text(result.response, NULL, 1, &handoff_validation);
               handoff_validation.repair_attempted = 1;
            }
            free(repaired.response);
         }
      }
      if (!handoff_validation.valid)
      {
         rc = 2;
         snprintf(result.error, sizeof(result.error), "invalid delegate handoff: %s",
                  handoff_validation.error[0] ? handoff_validation.error : "validation failed");
      }
   }
   /* Clear unconditionally: this thread is going back to the pool, and a leftover
    * override would silently re-scope the NEXT delegate's tools. */
   agent_tools_set_active_toolset(NULL);
   /* Reset the read-only gate unconditionally so the next user of this pooled
    * worker thread is not left in a prior delegate's (possibly read-only) state. */
   agent_tools_write_capable_set(1);
   if (parent_write_guard_active)
      agent_tools_parent_write_guard_clear();

   /* Server-initiated delegates review their own worktree (target is not
    * caller-supplied), so cwd-grounding applies. Threading a request-level
    * caller-provided-target signal here is a follow-up. */
   delegate_apply_review_evidence_guard(
       role, delegate_worktree_path[0] ? delegate_worktree_path : cwd, &rc, &result, 0);
   int delegate_applied_changes = -1;
   char delegate_apply_error[512] = "";
   char delegate_parent_root[MAX_PATH_LEN] = "";
   if (rc == 0 && delegate_allows_writes && delegate_worktree_path[0] && delegate_git_root[0] &&
       !delegate_shared_worktree)
   {
      /* The drift check guards against the PARENT worktree's HEAD moving during
       * the delegation, so its baseline must be the parent's HEAD at launch
       * (parent_worktree_head), not the delegate worktree's HEAD (launch_head).
       * The two normally match because the delegate worktree is branched from
       * the parent, but they diverge when the delegate worktree is cut from a
       * newer base (e.g. another session merged to main during the run) — in
       * which case using launch_head produces a false "parent HEAD changed"
       * refusal even though the parent never moved. */
      if (worktree_apply_delegate_changes_checked(
              delegate_worktree_path, cwd, parent_worktree_head, &delegate_applied_changes,
              delegate_parent_root, sizeof(delegate_parent_root), delegate_apply_error,
              sizeof(delegate_apply_error)) != 0)
      {
         rc = -1;
         result.success = 0;
         snprintf(result.error, sizeof(result.error), "delegate %s: %s", role,
                  delegate_apply_error[0] ? delegate_apply_error
                                          : "failed to apply changes to parent worktree");
      }
      else
         aimee_log(LOG_INFO, "delegate",
                   "delegate %s: applied %d change(s) from %s to parent worktree %s", deleg_id,
                   delegate_applied_changes, delegate_worktree_path, delegate_parent_root);
   }
   char stale_paths[8][DB1_SESSION_PATH_LEN];
   int n_stale = 0;
   if (sid && sid[0])
      n_stale = db1_session_stale_reads(sid, deleg_id, stale_paths, 8);
   if (rc == 0 && result.response && liveness_is_degenerate_response(result.response))
   {
      rc = -1;
      result.success = 0;
      snprintf(result.error, sizeof(result.error),
               "delegate %s returned raw tool-call markup or another degenerate response", role);
      free(result.response);
      result.response = NULL;
   }
   if (n_stale > 0 && result.response)
   {
      char note[2048];
      int off = snprintf(note, sizeof(note),
                         "\n\n[NOTE: subagent modified files the parent previously read — re-read"
                         " before editing:");
      for (int i = 0; i < n_stale && off < (int)sizeof(note) - 64; i++)
         off += snprintf(note + off, sizeof(note) - off, "%s %s", i ? "," : "", stale_paths[i]);
      snprintf(note + off, sizeof(note) - off, "]");

      size_t rl = strlen(result.response);
      size_t nl = strlen(note);
      char *augmented = malloc(rl + nl + 1);
      if (augmented)
      {
         memcpy(augmented, result.response, rl);
         memcpy(augmented + rl, note, nl + 1);
         free(result.response);
         result.response = augmented;
      }
   }

   if (sid && sid[0])
   {
      double child_cost = db1_token_audit_cost_for_delegation(deleg_id);
      if (child_cost > 0.0)
         (void)db1_cost_fold_record(sid, deleg_id, child_cost, "subagent");
   }

   cJSON *resp = delegate_build_result_response(deleg_id, rc, &result, &acfg, role, target_agent,
                                                delegate_applied_changes, handoff_checked,
                                                &handoff_validation, delegate_apply_error);
   delegate_checkout_add_result_ex(resp, launch_worktree_path, launch_head, parent_worktree_path,
                                   parent_worktree_head, parent_worktree_fingerprint);

   free(result.response);
   free(handoff_prompt);
   free(resolved_prompt);
   free(template_sys_prompt);
   free(learning_sys_prompt);
   compute_respond(cctx, resp);

   /* Unified-presence "speak first": tell the launching session's surfaces this
    * background delegate finished. presence_route_event publishes to the
    * session's event ring (so an attached /events SSE stream sees it) and
    * dispatches to any persistent messaging target (e.g. ntfy) the owner
    * registered. No-op when no presence is attached to the session. */
   if (effective_sid && effective_sid[0])
   {
      char summary[256];
      snprintf(summary, sizeof(summary), "delegate %s done — %d turns, %d tool calls",
               role && role[0] ? role : "task", result.turns, result.tool_calls);
      (void)presence_route_event(effective_sid, PRESENCE_EV_DELEGATE, "delegate_done", summary);
   }

   /* Classify and record a learning from this delegate exit. */
   delegate_record_exit_learning(sid, role, &result, rc, max_turns, &acfg, target_agent);

   /* Close the delegate_routing bandit decision (if one was sampled) with the run
    * outcome. By default success (rc == 0) -> 1.0, otherwise 0.0. When the
    * cost_reward flag is enabled, shape the success reward down by the delegate's
    * realized spend so the bandit prefers cheaper arms at comparable quality. */
   if (dr_decision_id[0] && dr_arm_id[0])
   {
      double reward = rc == 0 ? 1.0 : 0.0;
      if (rc == 0 && config_cost_reward_enabled())
      {
         double dcost = db1_token_audit_cost_for_delegation(deleg_id);
         reward = cost_shaped_reward(1, dcost, config_cost_reward_lambda_pct(),
                                     config_cost_reward_ref_usd_milli());
      }
      kb_client_bandit_close("delegate_routing", dr_decision_id, dr_arm_id, reward);
   }

   /* Reconcile delegate sibling-worktree edits via PR or supervisor review. */
   if (delegate_worktree_path[0] && delegate_git_root[0] && !delegate_allows_writes &&
       delegate_worktree_has_changes(delegate_worktree_path))
   {
      aimee_log(LOG_WARN, "delegate",
                "delegate %s produced changes for a read-only prompt; discarding worktree changes",
                deleg_id);
   }

   /* Clean up delegate worktree if we created one. */
   if (delegate_worktree_path[0] && delegate_git_root[0])
      worktree_cleanup(delegate_git_root, deleg_id, delegate_work_name);

   /* Release the credential lease so siblings can pick it up. If the
    * delegate failed with a rate-limit / 429 / overloaded error, cool
    * the leased credential first so the sibling that picks it up next
    * skips it until the upstream backs off. */
   if (target_agent && leased_cred_name[0])
   {
      failover_reason_t reason = FAILOVER_NONE;
      if (!result.success)
         reason = delegate_credentials_classify_failure(target_agent->provider, result.error);
      if (reason != FAILOVER_NONE)
         delegate_credentials_report_failure(leased_principal, target_agent->name, leased_cred_name,
                                             reason, result.error, time(NULL));
      delegate_credentials_release(leased_principal, target_agent->name, leased_cred_name);
      if (credential_state_path[0])
         (void)delegate_credentials_save_file(credential_state_path);
      leased_cred_name[0] = '\0';
      leased_principal[0] = '\0';
   }

   /* Clear thread-local CWD */
   run_cmd_set_cwd(NULL);

   compute_ctx_free(cctx);
   return;

   /* Single error-cleanup path for every pre-run error exit. Each action is
    * guarded by its own zero-initialised state, so an exit that never reached a
    * given acquisition is a no-op. Reached only by `goto delegate_fail` after
    * the exit has already sent its own error response; the success path returns
    * above and never falls through. */
delegate_fail:
   run_cmd_set_cwd(NULL);
   free(resolved_prompt);
   free(template_sys_prompt);
   if (target_agent && leased_cred_name[0])
      delegate_credentials_release(leased_principal, target_agent->name, leased_cred_name);
   if (delegate_worktree_path[0] && delegate_git_root[0])
      worktree_cleanup(delegate_git_root, deleg_id, delegate_work_name);
   compute_ctx_free(cctx);
}

int handle_delegate_launch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *plan = cJSON_GetObjectItemCaseSensitive(req, "plan");
   cJSON *jparallel = cJSON_GetObjectItemCaseSensitive(req, "parallel");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   int max_concurrent =
       cJSON_IsNumber(jparallel) ? (int)jparallel->valuedouble : DB1_COORD_DEFAULT_PAR;
   const char *cwd = (cJSON_IsString(jcwd) && jcwd->valuestring[0]) ? jcwd->valuestring : "";
   delegate_launch_result_t result;
   char err[256] = "";
   if (delegate_launch_coord_job(plan, max_concurrent, cwd, &result, err, sizeof(err)) != 0)
      return server_send_error(conn, err[0] ? err : "delegate launch failed", NULL);
   server_coord_dispatcher_notify();
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "plan_id", result.plan_id);
   cJSON_AddNumberToObject(resp, "job_id", result.job_id);
   cJSON_AddNumberToObject(resp, "tasks", result.tasks);
   cJSON_AddNumberToObject(resp, "max_concurrent", result.max_concurrent);
   cJSON_AddStringToObject(resp, "job_status", "pending");
   cJSON_AddStringToObject(resp, "status_command", "aimee job status <job_id>");
   return server_send_ok(conn, resp);
}

/* --- Public handlers (called from server dispatch) --- */

compute_ctx_t *create_compute_ctx(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   compute_ctx_t *cctx = calloc(1, sizeof(compute_ctx_t));
   if (!cctx)
      return NULL;

   cctx->server = ctx;
   cctx->async_slot = -1;
   /* dup() the fd so the compute worker holds its own reference.  If the
    * event loop closes conn->fd (client disconnect), the OS can recycle that
    * integer for the next accept().  Without dup, the worker would write
    * streaming output to an unrelated new connection — corrupting its auth
    * handshake and causing "server dropped connection" errors for other
    * clients.  With dup, close(conn->fd) merely frees the fd-table slot;
    * the underlying socket description stays alive until we close our copy. */
   /* A NULL conn is a server-initiated turn (no client socket): it streams to the
    * session's presence-event ring only. Used by chat.interrupt's auto-continue,
    * which dispatches the queued steer with no requesting connection. */
   if (!conn)
   {
      cctx->conn_fd = -1;
      cctx->conn_alive = 0;
   }
   else
   {
#ifdef AIMEE_POSIX
      int duped = dup(conn->fd);
      cctx->conn_fd = (duped >= 0) ? duped : conn->fd;
#else
      cctx->conn_fd = conn->fd;
#endif
      cctx->conn_alive = 1;
   }
   /* Capture the peer's caps for the foreign-cwd trust check (AC #6). A UDS peer
    * is CAPS_ALL (same filesystem); a remote/TCP peer is less, and its cwd must
    * not be opened on the server's own fs. A NULL conn is an in-process caller. */
   cctx->conn_caps = conn ? conn->capabilities : CAPS_ALL;

   /* WP-C vault identity: the conn's attested principal (hop 3) wins; else fall back
    * to the chat turn's per-turn thread-local — the chat-spawned delegate's loopback
    * conn carries no restored identity, and the TL is cleared per turn (no leak). */
   if (conn)
      cctx->attested_transport = conn->attested_transport;
   snprintf(cctx->vault_principal, sizeof(cctx->vault_principal), "%s",
            (conn && conn->vault_principal[0]) ? conn->vault_principal
                                               : agent_get_request_vault_principal());

   /* Clone the request since the original will be freed after dispatch */
   cctx->req = cJSON_Duplicate(req, 1);

   /* Create per-context write mutex */
   cctx->write_mutex = malloc(sizeof(pthread_mutex_t));
   pthread_mutex_init(cctx->write_mutex, NULL);

   return cctx;
}

int handle_tool_execute(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   compute_ctx_t *cctx = create_compute_ctx(ctx, conn, req);
   if (!cctx)
      return server_send_error(conn, "out of memory", NULL);

   if (tool_execute_dispatch(ctx, cctx) != 0)
   {
      compute_ctx_free(cctx);
      return server_send_error(conn, "tool queue full", NULL);
   }

   return 0; /* Response will be sent by worker thread */
}

/* enforce-delegate-only: provider the CORE gateway policy calls to learn whether
 * usable delegates exist (so it strips provider-native sub-agent tools from
 * proxied requests — Codex et al. can't spawn their own sub-agents). Reads
 * agents.json, so cache for a short TTL — it runs on the gateway hot path.
 * Registered by server_install_gateway_delegate_policy() at boot. */
static int gw_delegate_available_provider(void)
{
   static time_t cached_at = 0;
   static int cached = 0;
   time_t now = time(NULL);
   if (cached_at == 0 || now - cached_at > 30)
   {
      cached = agent_any_delegate_available();
      cached_at = now;
   }
   return cached;
}

void server_install_gateway_delegate_policy(void)
{
   gateway_policy_set_delegates_available_provider(gw_delegate_available_provider);
}

/* Conn-free async delegate launch: create a durable, pollable delegate job and
 * dispatch it detached, returning the job_id (>0) or -1 with `err`. Pass the
 * originating `conn` (its caps + vault principal are captured for the foreign-cwd
 * trust check) or NULL for a server-initiated launch (CAPS_ALL, per-turn vault
 * TL). Shared by handle_delegate (which then replies on conn) and the sub-agent
 * interceptor in the hook path. */
int server_delegate_launch_async(server_ctx_t *ctx, server_conn_t *conn, cJSON *req, char *err,
                                 size_t errn)
{
   compute_ctx_t *cctx = create_compute_ctx(ctx, conn, req);
   if (!cctx)
   {
      if (err)
         snprintf(err, errn, "out of memory");
      return -1;
   }

   cJSON *jrole = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   const char *role =
       delegate_role_canonicalize(cJSON_IsString(jrole) ? jrole->valuestring : "execute");
   const char *prompt = cJSON_IsString(jprompt) ? jprompt->valuestring : "";

   /* Cheap, request-only pre-flight validation (the persona check lives in the
    * MCP layer and the worker). */
   if (!prompt[0])
   {
      compute_ctx_free(cctx);
      if (err)
         snprintf(err, errn, "missing prompt");
      return -1;
   }
   if (strlen(prompt) < 20)
   {
      compute_ctx_free(cctx);
      if (err)
         snprintf(err, errn, "prompt too short (%zu chars)", strlen(prompt));
      return -1;
   }

   char lease_owner[32];
   snprintf(lease_owner, sizeof(lease_owner), "%d", (int)getpid());
   /* A positive `via` pin is already an assignment, even if execution must wait
    * for provider capacity. Persist it at creation so the Go control plane's
    * unassigned-job lease cannot cancel a correctly seated roundtable member
    * merely because its worker has not started yet. Generic routing remains
    * empty until the worker actually selects an eligible agent. */
   cJSON *jvia = cJSON_GetObjectItemCaseSensitive(req, "via");
   const char *initial_agent =
       cJSON_IsString(jvia) && jvia->valuestring[0] ? jvia->valuestring : "";
   int job_id = db1_agent_job_create(role, prompt, initial_agent, lease_owner);
   if (job_id <= 0)
   {
      compute_ctx_free(cctx);
      if (err)
         snprintf(err, errn, "failed to create delegate job");
      return -1;
   }

   cctx->background_job_id = job_id;
#ifdef AIMEE_POSIX
   if (cctx->conn_fd >= 0)
      close(cctx->conn_fd);
#endif
   cctx->conn_fd = -1;

   if (delegate_dispatch(ctx, cctx) != 0)
   {
      db1_agent_job_update(job_id, "failed", 0, "compute queue full");
      compute_ctx_free(cctx);
      if (err)
         snprintf(err, errn, "compute queue full");
      return -1;
   }
   return job_id;
}

int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   /* Async-only (WP-B): every delegate is a durable, pollable job. There is no
    * synchronous delegate path — the connection is closed at dispatch and the
    * caller polls delegate.status. The legacy `background` request field is
    * accepted-and-ignored for one release (older clients may still send it). */
   /* A caller that supplies an execution key is asking to replay its own step,
    * not to launch a second delegate for it. Resolve the reservation, launch,
    * and record it here, on the side that owns agent_jobs: when the ledger lived
    * across the network from the launch, a crash between the two left a
    * paid-for job that nothing could replay. */
   const char *execution_key = request_context_idempotency_key();
   char participant[DB1_AJ_PARTICIPANT_LEN] = "";
   int job_id = 0;
   int reservation_found =
       execution_key[0] &&
       db1_delegate_reservation_get(execution_key, &job_id, participant, sizeof(participant)) == 0;
   cJSON *replay_only = cJSON_GetObjectItemCaseSensitive(req, "replay_only");
   cJSON *work_item = cJSON_GetObjectItemCaseSensitive(req, "work_item_id");
   if (!reservation_found && cJSON_IsTrue(replay_only) && cJSON_IsString(work_item))
      reservation_found =
          db1_delegate_reservation_adopt_sole_legacy(execution_key, work_item->valuestring, &job_id,
                                                     participant, sizeof(participant)) == 0;
   if (reservation_found)
   {
      cJSON *replayed = jo_ok();
      cJSON_AddNumberToObject(replayed, "job_id", job_id);
      if (participant[0])
         cJSON_AddStringToObject(replayed, "participant", participant);
      cJSON_AddStringToObject(replayed, "job_status", "pending");
      cJSON_AddBoolToObject(replayed, "replayed", 1);
      return server_send_ok(conn, replayed);
   }

   /* A replay-only caller has already reconciled this step's spend. With no
    * reservation to replay there is nothing to serve, and launching would be
    * unreconciled duplicate spend, so report the absence instead. */
   if (cJSON_IsTrue(replay_only))
   {
      cJSON *absent = jo_ok();
      cJSON_AddNumberToObject(absent, "job_id", 0);
      cJSON_AddStringToObject(absent, "error", "no delegate reservation to replay");
      return server_send_ok(conn, absent);
   }

   char err[80] = "";
   job_id = server_delegate_launch_async(ctx, conn, req, err, sizeof(err));
   if (job_id <= 0)
      return server_send_error(conn, err[0] ? err : "failed to launch delegate", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_id", job_id);
   db1_agent_job_t job;
   if (db1_agent_job_get(job_id, &job) == 0)
   {
      if (job.participant_token[0])
      {
         snprintf(participant, sizeof(participant), "%s", job.participant_token);
         cJSON_AddStringToObject(resp, "participant", job.participant_token);
      }
      db1_agent_job_free(&job);
   }
   /* Reserve only after the launch produced a usable job id. A reservation
    * written first would make every retry replay a launch that never happened. */
   if (execution_key[0])
   {
      db1_delegate_reservation_save(execution_key,
                                    cJSON_IsString(work_item) ? work_item->valuestring : "", job_id,
                                    participant);
   }
   cJSON_AddStringToObject(resp, "job_status", "pending");
   return server_send_ok(conn, resp);
}

/* Resolve the one runtime panel contract shared by aggregate and roundtable.
 * A saved preset contributes its exact seats. Only the no-preset fallback is
 * synthesized, and that helper has a structural two-seat maximum. */
static int prepare_roundtable_panel(cJSON *req, ensemble_panel_t *panel, agent_config_t *acfg,
                                    char *err, size_t err_n)
{
   cJSON *jrt = cJSON_GetObjectItemCaseSensitive(req, "roundtable");
   if (jrt && !cJSON_IsString(jrt))
   {
      snprintf(err, err_n, "roundtable must name a saved preset");
      return -1;
   }
   const char *requested = cJSON_IsString(jrt) ? jrt->valuestring : NULL;
   return ensemble_prepare_runtime_panel(requested, panel, acfg, err, err_n);
}

/* Convene the ensemble panel from enabled registry agents when no explicit
 * ensemble.reference_models is set. Caps at ENSEMBLE_MAX_REFS; aggregator -> 0. */
/* Mixture-of-Agents ensemble aggregate. Reached over the first-class
 * POST /v1/delegate/aggregate route (method "delegate.aggregate"), dispatched
 * async via rh_dispatch_op_async onto a detached op-run worker (never the
 * buffered listener — the LLM fan-out may block here); result finalized into
 * /v1/runs/{id}. */
int handle_delegate_aggregate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   const char *prompt = cJSON_IsString(jprompt) ? jprompt->valuestring : "";
   if (!prompt || !prompt[0])
      return server_send_error(conn, "missing prompt", NULL);
   /* The ensemble prompt is the task; the same minimum-length guard as an
    * ordinary delegate applies, but it must name the ensemble. */
   if (strlen(prompt) < 20)
   {
      char errmsg[80];
      snprintf(errmsg, sizeof(errmsg), "ensemble prompt too short (%zu chars, min 20)",
               strlen(prompt));
      return server_send_error(conn, errmsg, NULL);
   }

   ensemble_panel_t panel;
   ensemble_panel_from_config(&panel);
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   if (agent_load_config(&acfg) != 0)
      return server_send_error(conn, "could not load agents.json", NULL);
   char pin_err[256] = "no enabled review agent is currently available";
   if (prepare_roundtable_panel(req, &panel, &acfg, pin_err, sizeof pin_err) != 0)
      return server_send_error(conn, pin_err, NULL);
   bind_request_session_creds(req);

   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &panel, prompt, &result);
   if (rc != 0)
      return server_send_error(conn, "ensemble run failed (no enabled agents in agents.json?)",
                               NULL);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "response", result.response);
   cJSON_AddBoolToObject(resp, "degraded", result.degraded ? 1 : 0);
   cJSON_AddBoolToObject(resp, "cost_capped", result.cost_capped ? 1 : 0);
   cJSON_AddNumberToObject(resp, "participants_total", result.participants_total);
   cJSON_AddNumberToObject(resp, "participants_failed", result.participants_failed);
   cJSON_AddNumberToObject(resp, "cost_usd", result.cost_usd);
   return server_send_ok(conn, resp);
}

int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "delegation_id");
   cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(req, "content");

   if (!cJSON_IsString(jid) || !cJSON_IsString(jcontent))
   {
      char augmented[1024];
      delegation_augment_error("missing delegation_id or content", augmented, sizeof(augmented));
      return server_send_error(conn, augmented, NULL);
   }

   delegation_mailbox_t *mb = mailbox_find(jid->valuestring);
   if (!mb)
   {
      char augmented[1024];
      delegation_augment_error("no active delegation with that ID", augmented, sizeof(augmented));
      return server_send_error(conn, augmented, NULL);
   }

   mailbox_reply(mb, jcontent->valuestring);
   cJSON *resp = jo_ok();
   return server_send_ok(conn, resp);
}
