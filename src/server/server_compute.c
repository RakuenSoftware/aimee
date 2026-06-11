/* server_compute.c: compute-layer handlers (tool.execute, delegate, chat.send_stream) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "db1.h"
#include "server_compute_impl.h"
#include "presence.h"
#include "compute_pool.h"
#include "agent.h"
#include "agent_coord.h"
#include "cmd_agent_delegate_impl.h"
#include "compute_concurrency.h"
#include "config.h"
#include "delegate_credential_retry.h"
#include "delegate_launch.h"
#include "delegate_source_authority.h"
#include "server_coord_dispatcher.h"
#include "delegate_credentials.h"
#include "delegate_economics.h"
#include "delegate_run_phases.h"
#include "db1/delegate_learning.h"
#include "kb_client.h"
#include "kb_bandit.h"
#include "db1/interaction_events.h"
#include "delegate_role.h"
#include "delegate_ensemble.h"
#include "guardrails.h"
#include "liveness.h"
#include "log.h"
#include "model_registry.h"
#include "openai_runs_store.h"
#include "platform_process.h"
#include "prompts.h"
#include "persona.h"
#include "server_http.h"
#include "provider_catalog.h"
#include "role_templates.h"
#include "workspace.h"
#include "workspace_provider.h"
#include "workspace_turn.h"
#include "cJSON.h"

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
#define DELEGATION_INPUT_TIMEOUT 60 /* seconds */
#define MAX_ACTIVE_DELEGATIONS   32
/* Delegation mailbox: allows delegates to pause and receive parent replies */
typedef struct
{
   char delegation_id[64];
   pthread_mutex_t lock;
   pthread_cond_t reply_ready;
   char reply[4096];
   int has_reply;
   int active;
} delegation_mailbox_t;

static delegation_mailbox_t g_mailboxes[MAX_ACTIVE_DELEGATIONS];
static pthread_mutex_t g_mailbox_lock = PTHREAD_MUTEX_INITIALIZER;
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

#include "server_compute_concurrency.inc"

static delegation_mailbox_t *mailbox_acquire(const char *delegation_id)
{
   pthread_mutex_lock(&g_mailbox_lock);
   for (int i = 0; i < MAX_ACTIVE_DELEGATIONS; i++)
   {
      if (!g_mailboxes[i].active)
      {
         delegation_mailbox_t *mb = &g_mailboxes[i];
         snprintf(mb->delegation_id, sizeof(mb->delegation_id), "%s", delegation_id);
         pthread_mutex_init(&mb->lock, NULL);
         pthread_cond_init(&mb->reply_ready, NULL);
         mb->reply[0] = '\0';
         mb->has_reply = 0;
         mb->active = 1;
         pthread_mutex_unlock(&g_mailbox_lock);
         return mb;
      }
   }
   pthread_mutex_unlock(&g_mailbox_lock);
   return NULL;
}

static int delegate_agent_uses_mistral_path(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (strcmp(agent->provider, "mistral") == 0)
      return 1;
   return strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 &&
          (strcmp(agent->cli_kind, "mistral") == 0 ||
           strcmp(agent->cli_kind, "mistral-plan") == 0 || strcmp(agent->cli_kind, "vibe") == 0 ||
           strcmp(agent->cli_kind, "vibe-plan") == 0);
}

static void mailbox_release(delegation_mailbox_t *mb)
{
   if (!mb)
      return;
   pthread_mutex_lock(&g_mailbox_lock);
   mb->active = 0;
   mb->delegation_id[0] = '\0';
   pthread_mutex_destroy(&mb->lock);
   pthread_cond_destroy(&mb->reply_ready);
   pthread_mutex_unlock(&g_mailbox_lock);
}

static delegation_mailbox_t *mailbox_find(const char *delegation_id)
{
   pthread_mutex_lock(&g_mailbox_lock);
   for (int i = 0; i < MAX_ACTIVE_DELEGATIONS; i++)
   {
      if (g_mailboxes[i].active && strcmp(g_mailboxes[i].delegation_id, delegation_id) == 0)
      {
         delegation_mailbox_t *mb = &g_mailboxes[i];
         pthread_mutex_unlock(&g_mailbox_lock);
         return mb;
      }
   }
   pthread_mutex_unlock(&g_mailbox_lock);
   return NULL;
}

/* Wait for a parent reply with timeout. Returns 0 on reply, -1 on timeout. */
static int mailbox_wait(delegation_mailbox_t *mb, char *out, size_t out_len, int timeout_secs)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += timeout_secs;

   pthread_mutex_lock(&mb->lock);
   while (!mb->has_reply)
   {
      int rc = pthread_cond_timedwait(&mb->reply_ready, &mb->lock, &ts);
      if (rc != 0) /* ETIMEDOUT or error */
      {
         pthread_mutex_unlock(&mb->lock);
         return -1;
      }
   }
   snprintf(out, out_len, "%s", mb->reply);
   mb->has_reply = 0;
   mb->reply[0] = '\0';
   pthread_mutex_unlock(&mb->lock);
   return 0;
}

/* Send a reply to a waiting delegate. */
static void mailbox_reply(delegation_mailbox_t *mb, const char *content)
{
   pthread_mutex_lock(&mb->lock);
   snprintf(mb->reply, sizeof(mb->reply), "%s", content);
   mb->has_reply = 1;
   pthread_cond_signal(&mb->reply_ready);
   pthread_mutex_unlock(&mb->lock);
}

/* Global: current delegation mailbox for the calling thread (used by tool_request_input) */
static __thread delegation_mailbox_t *tl_mailbox = NULL;

/* Delegation depth tracking: parent delegation ID and current depth for this thread */
static __thread char tl_parent_delegation_id[64] = {0};
static __thread int tl_delegation_depth = 0;

/* Returns the delegation ID of the currently-running delegate on this
 * thread, or NULL when not inside a delegate. Provides agent_tools_dispatch
 * with a write-tracking key that's distinct from the caller's session_id —
 * lets db1_session_stale_reads tell "the child wrote what the parent read"
 * apart from "this session read and wrote the same file." */
const char *delegation_active_id(void)
{
   return tl_parent_delegation_id[0] ? tl_parent_delegation_id : NULL;
}

/* Called by tool_request_input from within the delegate agent */
char *delegation_request_input(const char *question)
{
   if (!tl_mailbox || !tl_mailbox->active)
      return NULL;

   /* Record the question */
   (void)db1_delegation_message_record(tl_mailbox->delegation_id, "delegate_to_parent", question);

   /* Wait for parent reply */
   char reply[4096];
   if (mailbox_wait(tl_mailbox, reply, sizeof(reply), DELEGATION_INPUT_TIMEOUT) != 0)
      return NULL; /* timeout */

   /* Record the reply */
   (void)db1_delegation_message_record(tl_mailbox->delegation_id, "parent_to_delegate", reply);

   size_t len = strlen(reply);
   char *out = malloc(len + 1);
   if (out)
      memcpy(out, reply, len + 1);
   return out;
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
   }

   db1_agent_job_update(cctx->background_job_id, job_status, cursor_turn, result);
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
      db1_coord_job_complete_task(cctx->coord_task_id, cJSON_IsString(r) ? r->valuestring : "");
   }
   else if (strcmp(status_text, "preempted") == 0)
   {
      config_t cfg;
      config_load(&cfg);
      if (db1_coord_job_release_task_bounded(cctx->coord_task_id,
                                             cfg.concurrency_preempt_requeue_max) == 0)
      {
         server_coord_dispatcher_notify();
         return;
      }
      db1_coord_job_fail_task(cctx->coord_task_id, "preempt requeue cap exhausted");
   }
   else
   {
      cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
      db1_coord_job_fail_task(cctx->coord_task_id, (cJSON_IsString(m) && m->valuestring[0])
                                                       ? m->valuestring
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

   /* Sessionless/background delegates run on the global background pool. */
   return compute_pool_submit(&ctx->pool, delegate_worker, cctx);
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

static int delegate_request_priority(const compute_ctx_t *cctx, cJSON *jpriority)
{
   int priority = cctx && cctx->background_job_id > 0 ? CONCURRENCY_PRIORITY_BACKGROUND
                                                      : CONCURRENCY_PRIORITY_INTERACTIVE;
   if (cJSON_IsNumber(jpriority))
      priority = (int)jpriority->valuedouble;
   if (priority < -1000)
      priority = -1000;
   if (priority > 1000)
      priority = 1000;
   return priority;
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

   delegate_source_env_capture(&c->source_env);
   delegate_source_env_clear_for_worktree(source_env_root);
}

static void delegate_run_ctx_restore(const delegate_run_ctx_t *c)
{
   platform_setenv("AIMEE_DELEGATE_DEPTH", c->saved_env_depth[0] ? c->saved_env_depth : "");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", c->saved_env_parent[0] ? c->saved_env_parent : "");
   delegate_source_env_restore(&c->source_env);
   tl_delegation_depth = c->saved_depth;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "%s", c->saved_parent);

   session_id_clear_override();
   tl_mailbox = NULL;
   mailbox_release(c->mb);
}

/* Build the delegate result envelope from a finished run. Mirrors the two
 * branches the worker emitted inline: on success the response + turn/token
 * metrics; on failure the (possibly stop-reason) status + augmented message,
 * with the response and apply_error attached when present. Economics and
 * handoff-validation fields are added the same way for both. Returns a new
 * cJSON object; the caller attaches checkout info and responds. */
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
      if (result->response)
         cJSON_AddStringToObject(resp, "response", result->response);
      if (apply_error[0])
         cJSON_AddStringToObject(resp, "apply_error", apply_error);
      if (handoff_checked)
         delegate_handoff_add_validation_json(resp, handoff_validation);
   }
   return resp;
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
   cJSON *req = cctx->req;
   /* Per-turn credential context: credential-session id (RAM keyring; a
    * dedicated field decoupled from the chat session id) + any per-turn Codex
    * creds (legacy direct push). Empty/absent clears them. */
   {
      const char *cred_sid = jo_str(req, "cred_session_id", NULL);
      agent_set_request_session((cred_sid && cred_sid[0]) ? cred_sid
                                                          : compute_request_session_id(req));
   }
   agent_set_request_codex_creds(jo_str(req, "codex_oauth_token", NULL),
                                 jo_str(req, "codex_account_id", NULL));
   cJSON *jrole = cJSON_GetObjectItemCaseSensitive(req, "role");
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   cJSON *jmax = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   cJSON *jsystem = cJSON_GetObjectItemCaseSensitive(req, "system_prompt");
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "delegation_id");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(req, "branch");
   cJSON *jtimeout = cJSON_GetObjectItemCaseSensitive(req, "timeout_ms");
   cJSON *jmaxturns = cJSON_GetObjectItemCaseSensitive(req, "max_turns");
   cJSON *jhandoff = cJSON_GetObjectItemCaseSensitive(req, "handoff_json");
   cJSON *jtools = cJSON_GetObjectItemCaseSensitive(req, "tools");
   cJSON *jtier = cJSON_GetObjectItemCaseSensitive(req, "tier");
   cJSON *jvia = cJSON_GetObjectItemCaseSensitive(req, "via");
   cJSON *jprovider = cJSON_GetObjectItemCaseSensitive(req, "provider");
   cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(req, "model");
   cJSON *jparent_deleg = cJSON_GetObjectItemCaseSensitive(req, "parent_delegation_id");
   cJSON *jpriority = cJSON_GetObjectItemCaseSensitive(req, "priority");
   cJSON *jreq_caps = cJSON_GetObjectItemCaseSensitive(req, "required_caps");
   cJSON *jmin_ctx = cJSON_GetObjectItemCaseSensitive(req, "min_context");
   const char *role =
       delegate_role_canonicalize(cJSON_IsString(jrole) ? jrole->valuestring : "execute");
   const char *prompt = cJSON_IsString(jprompt) ? jprompt->valuestring : "";
   int max_tokens = cJSON_IsNumber(jmax) ? (int)jmax->valuedouble : 4096;
   const char *system_prompt = cJSON_IsString(jsystem) ? jsystem->valuestring : NULL;
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : NULL;
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";
   const char *branch =
       (cJSON_IsString(jbranch) && jbranch->valuestring[0]) ? jbranch->valuestring : NULL;
   int timeout_ms = cJSON_IsNumber(jtimeout) ? (int)jtimeout->valuedouble : 0;
   int max_turns = cJSON_IsNumber(jmaxturns) ? (int)jmaxturns->valuedouble : -1;
   int handoff_json = cJSON_IsTrue(jhandoff);
   const char *toolset_override =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "toolset"));
   int tier_override = cJSON_IsNumber(jtier) ? (int)jtier->valuedouble : -1;
   const char *via_name = cJSON_IsString(jvia) ? jvia->valuestring : NULL;
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
   int delegate_priority = delegate_request_priority(cctx, jpriority);
   int explicit_tools = cJSON_IsTrue(jtools),
       force_tools = delegate_role_auto_tools_for_invocation(role, max_turns, explicit_tools);
   force_tools = force_tools || (toolset_override && toolset_override[0]);
   /* Generate delegation ID if not provided */
   char deleg_id[64];
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
    * as the primary chat path does. Gated by claude_cli_delegate_enabled. */
   if (via_name && via_name[0])
   {
      agent_t *cag = agent_find(&acfg, via_name);
      if (cag && agent_is_claude_cli(cag))
      {
         int detached_bound = workspace_turn_bind_active(cwd);
         const workspace_provider_t *wsp = workspace_provider_active();
         if (detached_bound && wsp && wsp->kind == WS_PROVIDER_DETACHED && wsp->exec_shell)
         {
            config_t gate_cfg;
            config_load(&gate_cfg);
            if (!gate_cfg.claude_cli_delegate_enabled)
            {
               workspace_turn_unbind_active();
               char m[320];
               snprintf(m, sizeof(m),
                        "agent '%s' is Claude via the `claude` CLI and is primary-only by default; "
                        "set claude_cli_delegate_enabled=true to allow it as a delegate "
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
            int rc = agent_execute_cli_session(cag, NULL, sysp ? sysp : "", prompt,
                                               AGENT_DEFAULT_MAX_TOKENS, 0.3, &result);
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
         config_t dr_cfg;
         config_load(&dr_cfg);
         if (dr_cfg.bandit_live_decision_enabled)
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

   config_t route_cfg;
   config_load(&route_cfg);
   agent_t *target_agent =
       agent_route_with_caps(&acfg, role, &route_cfg, required_caps, min_context);
   if (!target_agent)
   {
      char caps_buf[128];
      model_capability_format_flags(required_caps, caps_buf, sizeof(caps_buf));
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg),
               "no configured model supports required capabilities (caps=%s, min_context=%d)",
               caps_buf[0] ? caps_buf : "none", min_context);
      delegation_compute_error(cctx, errmsg);
      compute_ctx_free(cctx);
      return;
   }
   /* Claude run via the `claude` CLI/tmux login (not an API key) is primary-only
    * by default: driving a personal Claude subscription as an automated delegate
    * may breach Anthropic's terms. Opt in with `claude_cli_delegate_enabled`.
    * (Concise here — the risk warning is shown once at setup when the flag is
    * enabled, and in DELEGATES.md.) */
   if (agent_is_claude_cli(target_agent) && !route_cfg.claude_cli_delegate_enabled)
   {
      char errmsg[320];
      snprintf(errmsg, sizeof(errmsg),
               "agent '%s' is Claude via the `claude` CLI and is primary-only by default; "
               "set claude_cli_delegate_enabled=true to allow it as a delegate "
               "(see DELEGATES.md for the Anthropic account-risk warning)",
               target_agent->name);
      delegation_compute_error(cctx, errmsg);
      compute_ctx_free(cctx);
      return;
   }
   if (target_agent && timeout_ms > 0)
      target_agent->timeout_ms = timeout_ms;
   delegate_apply_max_turns_policy(&acfg, role, max_turns);
   if (cctx->background_job_id > 0 && target_agent)
      db1_agent_job_set_agent(cctx->background_job_id, target_agent->name);
   /* Lease one credential from a configured pool for this delegate run. */
   char leased_cred_name[MAX_CRED_NAME_LEN] = "";
   char credential_state_path[MAX_PATH_LEN] = "";
   if (target_agent && target_agent->credential_count > 0)
   {
      char leased_env[MAX_CRED_ENV_VAR_LEN] = "";
      const char *dir = config_output_dir();
      snprintf(credential_state_path, sizeof(credential_state_path),
               "%s/delegate-credential-state.tsv", dir ? dir : "/tmp");
      (void)delegate_credentials_load_file(credential_state_path, time(NULL));
      if (delegate_credentials_acquire(
              target_agent->name, target_agent->credentials, target_agent->credential_count,
              leased_cred_name, sizeof(leased_cred_name), leased_env, sizeof(leased_env)) == 0)
      {
         const char *value = getenv(leased_env);
         if (value && value[0])
            snprintf(target_agent->api_key, MAX_API_KEY_LEN, "%s", value);
      }
      else
      {
         char errmsg[256];
         snprintf(errmsg, sizeof(errmsg), "no available credential in pool for agent '%s'",
                  target_agent->name);
         delegation_compute_error(cctx, errmsg);
         compute_ctx_free(cctx);
         return;
      }
   }

   /* Enforce delegation depth limit.
    *
    * For in-process sub-delegations (agent using mcp__aimee__delegate), the
    * thread-local tl_delegation_depth is authoritative. For cross-process
    * chains (agent shells out to aimee-client), the client sends
    * AIMEE_DELEGATE_DEPTH/AIMEE_PARENT_DELEGATION_ID as request fields. Stale
    * completed parents are ignored so a primary shell with leaked env does not
    * get misclassified as a live delegate. */
   config_t cfg;
   config_load(&cfg);
   int max_depth = cfg.max_delegation_depth > 0 ? cfg.max_delegation_depth
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
      compute_ctx_free(cctx);
      return;
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
   int max_spawns = cfg.max_delegation_spawns > 0 ? cfg.max_delegation_spawns
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
         compute_ctx_free(cctx);
         return;
      }
   }

   /* Resolve @path/to/file references in the delegate prompt */
   char *resolved_prompt = NULL;
   if (strchr(prompt, '@'))
   {
      resolved_prompt = resolve_file_references(prompt, cwd[0] ? cwd : ".");
      if (resolved_prompt)
         prompt = resolved_prompt;
   }
   int role_allows_writes = delegate_role_is_write(role);
   int delegate_allows_writes = role_allows_writes && delegate_prompt_allows_writes(prompt);
   if (branch && !delegate_allows_writes)
   {
      free(resolved_prompt);
      if (target_agent && leased_cred_name[0])
         delegate_credentials_release(target_agent->name, leased_cred_name);
      delegation_compute_error(cctx, "read-only delegates must use the parent worktree; branch "
                                     "requests require a sibling delegate worktree");
      compute_ctx_free(cctx);
      return;
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

   /* Read-only delegates use parent workspace; write-capable delegates use sibling worktrees. */
   char delegate_worktree_path[MAX_PATH_LEN] = "";
   char delegate_git_root[MAX_PATH_LEN] = "";
   char delegate_work_name[32] = "";
   int delegate_worktree_attempted = 0;
   int delegate_shared_worktree = 0;
   {
      delegate_worktree_t wt;
      delegate_resolve_worktree(cwd, deleg_id, branch, delegate_allows_writes,
                                delegate_needs_worktree, &wt);
      snprintf(delegate_worktree_path, sizeof(delegate_worktree_path), "%s", wt.worktree_path);
      snprintf(delegate_git_root, sizeof(delegate_git_root), "%s", wt.git_root);
      snprintf(delegate_work_name, sizeof(delegate_work_name), "%s", wt.work_name);
      delegate_worktree_attempted = wt.attempted;
      delegate_shared_worktree = wt.shared;
   }

   if (delegate_allows_writes && delegate_worktree_attempted && !delegate_worktree_path[0])
   {
      char errmsg[512];
      snprintf(errmsg, sizeof(errmsg),
               "refusing to run write-capable delegate in parent worktree '%s': "
               "could not create an isolated delegate worktree",
               cwd[0] ? cwd : delegate_git_root);
      free(resolved_prompt);
      if (target_agent && leased_cred_name[0])
      {
         delegate_credentials_release(target_agent->name, leased_cred_name);
         leased_cred_name[0] = '\0';
      }
      run_cmd_set_cwd(NULL);
      delegation_compute_error(cctx, errmsg);
      compute_ctx_free(cctx);
      return;
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
   char *template_sys_prompt = NULL;
   system_prompt =
       delegate_assemble_system_prompt(system_prompt, role, prompt, cwd, persona_override,
                                       delegate_worktree_path, &template_sys_prompt);

   /* Ground read-only inspection roles in parent diff evidence. */
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
    * already loaded.  Silently skips if kb is unreachable. */
   {
      char *ctx = delegate_inject_code_context(prompt);
      if (ctx)
      {
         char *combined = delegate_prompt_append_block(system_prompt, ctx);
         if (combined)
         {
            free(template_sys_prompt);
            template_sys_prompt = combined;
            system_prompt = combined;
         }
         free(ctx);
      }
   }

   /* Tier orchestration context: for tools-enabled mid-tier delegates, append
    * instructions describing how to fan out sub-tasks to lower-tier agents. */
   {
      char *tier_ctx = delegate_build_tier_context(via_name, tier_override, role);
      if (tier_ctx)
      {
         char *combined = delegate_prompt_append_block(system_prompt, tier_ctx);
         if (combined)
         {
            free(template_sys_prompt);
            template_sys_prompt = combined;
            system_prompt = combined;
         }
         free(tier_ctx);
      }
   }

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
   int is_write_role = delegate_role_is_write(role);
   if (is_write_role)
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
      int drift_rc = delegate_check_named_file_drift(path_ptrs, named_path_count, prompt, NULL,
                                                     drift_root, drift_err, sizeof(drift_err));
      if (drift_rc < 0)
      {
         aimee_log(LOG_WARN, "delegate", "named-file drift guard (pre-flight): %s", drift_err);
         if (delegate_worktree_path[0] && delegate_git_root[0])
            worktree_cleanup(delegate_git_root, deleg_id, delegate_work_name);
         free(template_sys_prompt);
         free(resolved_prompt);
         if (target_agent && leased_cred_name[0])
         {
            delegate_credentials_release(target_agent->name, leased_cred_name);
            leased_cred_name[0] = '\0';
         }
         run_cmd_set_cwd(NULL);
         cJSON *eresp = cJSON_CreateObject();
         cJSON_AddStringToObject(eresp, "delegation_id", deleg_id);
         cJSON_AddStringToObject(eresp, "status", "error");
         cJSON_AddStringToObject(eresp, "message", drift_err);
         compute_respond(cctx, eresp);
         compute_ctx_free(cctx);
         return;
      }
   }

   delegate_run_ctx_t run_ctx;
   delegate_run_ctx_enter(&run_ctx, deleg_id, sid, current_depth,
                          delegate_worktree_path[0] ? delegate_worktree_path : (cwd[0] ? cwd : ""));
   (void)db1_delegation_spawn_record(deleg_id, effective_parent, effective_sid, current_depth,
                                     role);

   /* Free-routed delegates use a shared tier pool; explicit --via stays per-model. */
   concurrency_ensure_current();
   char conc_tier_key[16] = "";
   concurrency_route_key_t conc =
       concurrency_key(via_name, target_agent, conc_tier_key, sizeof(conc_tier_key));
   concurrency_maybe_preempt_delegate(conc.model, conc.provider, delegate_priority, deleg_id);
   if (cctx->background_job_id > 0 && conc.model[0])
      db1_agent_job_heartbeat_ext(cctx->background_job_id, "concurrency_slot", 0);
   concurrency_acquire_status_t conc_status = CONCURRENCY_ACQUIRE_BYPASS;
   concurrency_slot_t *conc_slot = concurrency_acquire_priority_owner_cancellable_status(
       &g_concurrency_mgr, conc.model, conc.provider, delegate_priority, deleg_id,
       db1_delegation_spawn_is_stopped, deleg_id, &conc_status);
   if (!conc_slot && conc.model[0] && conc_status != CONCURRENCY_ACQUIRE_BYPASS)
   {
      const int queue_full = conc_status == CONCURRENCY_ACQUIRE_QUEUE_FULL;
      aimee_log(queue_full ? LOG_WARN : LOG_INFO, "delegate",
                queue_full ? "delegation %s rejected: concurrency queue full for %s"
                           : "delegation %s cancelled while queued for concurrency slot",
                deleg_id, conc.model);
      delegate_run_ctx_restore(&run_ctx);
      free(resolved_prompt);
      cJSON *cresp = cJSON_CreateObject();
      cJSON_AddStringToObject(cresp, "delegation_id", deleg_id);
      cJSON_AddStringToObject(cresp, "status", queue_full ? "error" : "cancelled");
      cJSON_AddStringToObject(cresp, "message",
                              queue_full
                                  ? "delegate concurrency queue full"
                                  : "delegation cancelled while waiting for concurrency slot");
      compute_respond(cctx, cresp);
      if (delegate_worktree_path[0] && delegate_git_root[0])
         worktree_cleanup(delegate_git_root, deleg_id, delegate_work_name);
      free(template_sys_prompt);
      if (target_agent && leased_cred_name[0])
      {
         delegate_credentials_release(target_agent->name, leased_cred_name);
         leased_cred_name[0] = '\0';
      }
      run_cmd_set_cwd(NULL);
      compute_ctx_free(cctx);
      return;
   }
   if (cctx->background_job_id > 0 && conc.model[0])
      db1_agent_job_heartbeat_ext(cctx->background_job_id, "", 0);
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

   /* Provider-backed delegates can call back into aimee-server tools while
    * waiting on the model. Keep the server compute budget available for
    * those callbacks; delegate concurrency is already governed above by the
    * per-model/provider limiter. */
   compute_ctx_release_budget(cctx);

   if (cwd[0])
   {
      const char *write_root = delegate_worktree_path[0] ? delegate_worktree_path : NULL;
      agent_tools_parent_write_guard_set(cwd, write_root);
      parent_write_guard_active = 1;
   }

   int rc;
   const char *saved_toolset_env = getenv("AIMEE_ACTIVE_TOOLSET");
   char saved_toolset_buf[128] = "";
   if (saved_toolset_env && saved_toolset_env[0])
      snprintf(saved_toolset_buf, sizeof(saved_toolset_buf), "%s", saved_toolset_env);
   platform_setenv("AIMEE_ACTIVE_TOOLSET", toolset_override ? toolset_override : "");
   rc = delegate_run_with_credential_retry(&acfg, target_agent, role, system_prompt, run_prompt,
                                           max_tokens, force_tools, delegate_allows_writes,
                                           leased_cred_name, sizeof(leased_cred_name),
                                           credential_state_path, &result);
   concurrency_release_owner(conc_slot, deleg_id);
   delegate_run_ctx_restore(&run_ctx);
   (void)db1_delegation_spawn_complete(deleg_id);

   /* Post-run named-file drift check: verify named existing paths appear in response. */
   if (named_path_count > 0 && rc == 0 && result.response && result.response[0])
   {
      const char *path_ptrs[DELEGATE_DRIFT_MAX_PATHS];
      for (int i = 0; i < named_path_count; i++)
         path_ptrs[i] = named_paths[i];
      char drift_err[512];
      int drift_rc = delegate_check_named_file_drift(
          path_ptrs, named_path_count, prompt, result.response,
          delegate_worktree_path[0] ? delegate_worktree_path : NULL, drift_err, sizeof(drift_err));
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
      if (delegate_detect_noop_write(is_write_role, delegate_allows_writes, handoff_json, rc,
                                     named_paths, named_path_count, pre_run_files, pre_run_head_sha,
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
   platform_setenv("AIMEE_ACTIVE_TOOLSET", saved_toolset_buf);
   if (parent_write_guard_active)
      agent_tools_parent_write_guard_clear();
   delegate_apply_review_evidence_guard(
       role, delegate_worktree_path[0] ? delegate_worktree_path : cwd, &rc, &result);
   int delegate_applied_changes = -1;
   char delegate_apply_error[512] = "";
   char delegate_parent_root[MAX_PATH_LEN] = "";
   if (rc == 0 && is_write_role && delegate_allows_writes && delegate_worktree_path[0] &&
       delegate_git_root[0] && !delegate_shared_worktree)
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
    * outcome: success (rc == 0) -> 1.0, otherwise 0.0. Best-effort. */
   if (dr_decision_id[0] && dr_arm_id[0])
      kb_client_bandit_close("delegate_routing", dr_decision_id, dr_arm_id, rc == 0 ? 1.0 : 0.0);

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
         delegate_credentials_report_failure(target_agent->name, leased_cred_name, reason,
                                             result.error, time(NULL));
      delegate_credentials_release(target_agent->name, leased_cred_name);
      if (credential_state_path[0])
         (void)delegate_credentials_save_file(credential_state_path);
      leased_cred_name[0] = '\0';
   }

   /* Clear thread-local CWD */
   run_cmd_set_cwd(NULL);

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
#ifdef AIMEE_POSIX
   {
      int duped = dup(conn->fd);
      cctx->conn_fd = (duped >= 0) ? duped : conn->fd;
   }
#else
   cctx->conn_fd = conn->fd;
#endif
   cctx->conn_alive = 1;
   /* Capture the peer's caps for the foreign-cwd trust check (AC #6). A UDS peer
    * is CAPS_ALL (same filesystem); a remote/TCP peer is less, and its cwd must
    * not be opened on the server's own fs. A NULL conn is an in-process caller. */
   cctx->conn_caps = conn ? conn->capabilities : CAPS_ALL;

   /* Clone the request since the original will be freed after dispatch */
   cctx->req = cJSON_Duplicate(req, 1);

   /* Create per-context write mutex */
   cctx->write_mutex = malloc(sizeof(pthread_mutex_t));
   pthread_mutex_init(cctx->write_mutex, NULL);

   return cctx;
}

static int roundtable_run_cancel_requested(void *ctx)
{
   const char *run_id = (const char *)ctx;
   return run_id && run_id[0] && openai_runs_store_cancel_requested(run_id);
}

#include "server_compute_roundtable.inc"

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

int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   compute_ctx_t *cctx = create_compute_ctx(ctx, conn, req);
   if (!cctx)
      return server_send_error(conn, "out of memory", NULL);

   cJSON *jbackground = cJSON_GetObjectItemCaseSensitive(req, "background");
   if (cJSON_IsTrue(jbackground))
   {
      cJSON *jrole = cJSON_GetObjectItemCaseSensitive(req, "role");
      cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
      const char *role =
          delegate_role_canonicalize(cJSON_IsString(jrole) ? jrole->valuestring : "execute");
      const char *prompt = cJSON_IsString(jprompt) ? jprompt->valuestring : "";

      if (!prompt[0])
      {
         compute_ctx_free(cctx);
         return server_send_error(conn, "missing prompt", NULL);
      }
      if (strlen(prompt) < 20)
      {
         char errmsg[64];
         snprintf(errmsg, sizeof(errmsg), "prompt too short (%zu chars)", strlen(prompt));
         compute_ctx_free(cctx);
         return server_send_error(conn, errmsg, NULL);
      }

      char lease_owner[32];
      snprintf(lease_owner, sizeof(lease_owner), "%d", (int)getpid());
      int job_id = db1_agent_job_create(role, prompt, "", lease_owner);
      if (job_id <= 0)
      {
         compute_ctx_free(cctx);
         return server_send_error(conn, "failed to create delegate job", NULL);
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
         return server_send_error(conn, "compute queue full", NULL);
      }

      cJSON *resp = jo_ok();
      cJSON_AddNumberToObject(resp, "job_id", job_id);
      cJSON_AddStringToObject(resp, "job_status", "pending");
      return server_send_ok(conn, resp);
   }

   if (delegate_dispatch(ctx, cctx) != 0)
   {
      compute_ctx_free(cctx);
      return server_send_error(conn, "compute queue full", NULL);
   }

   return 0; /* Response will be sent by worker thread */
}

/* Mixture-of-Agents ensemble aggregate. Reached over the first-class
 * POST /v1/delegate/aggregate route (method "delegate.aggregate"), dispatched
 * async via rh_dispatch_op_async — so this runs on a detached op-run worker
 * thread, never the buffered listener, and the LLM fan-out may block here. The
 * run result is finalized into /v1/runs/{id}. Before this entry point existed,
 * delegate_ensemble_run had no caller in any shipped binary. */
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

   config_t cfg;
   config_load(&cfg);
   if (!cfg.ensemble_enabled)
      return server_send_error(
          conn, "Mixture-of-Agents ensemble disabled (set ensemble.enabled=true)", NULL);

   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   if (agent_load_config(&acfg) != 0)
      return server_send_error(conn, "could not load agents.json", NULL);

   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, prompt, &result);
   if (rc != 0)
      return server_send_error(
          conn, "ensemble run failed (check ensemble.enabled / ensemble.reference_models)", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "response", result.response);
   cJSON_AddBoolToObject(resp, "degraded", result.degraded ? 1 : 0);
   cJSON_AddBoolToObject(resp, "cost_capped", result.cost_capped ? 1 : 0);
   cJSON_AddNumberToObject(resp, "cost_usd", result.cost_usd);
   return server_send_ok(conn, resp);
}

int handle_delegate_roundtable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   const char *prompt = cJSON_IsString(jprompt) ? jprompt->valuestring : "";
   if (!prompt || !prompt[0])
      return server_send_error(conn, "missing prompt", NULL);
   if (strlen(prompt) < 20)
   {
      char errmsg[88];
      snprintf(errmsg, sizeof(errmsg), "roundtable prompt too short (%zu chars, min 20)",
               strlen(prompt));
      return server_send_error(conn, errmsg, NULL);
   }

   config_t cfg;
   config_load(&cfg);
   if (!cfg.ensemble_enabled)
      return server_send_error(conn, "agent roundtable disabled (set ensemble.enabled=true)", NULL);

   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = strcmp(cfg.roundtable_turns, "sequential") == 0 ? ROUNDTABLE_SEQUENTIAL
                                                                : ROUNDTABLE_PARALLEL;
   opts.max_rounds = cfg.roundtable_max_rounds > 0 ? cfg.roundtable_max_rounds : 3;
   opts.converge_threshold = cfg.roundtable_converge_threshold;
   opts.deadline_ms = cfg.roundtable_deadline_ms;
   cJSON *jrun = cJSON_GetObjectItemCaseSensitive(req, "__run_id");
   if (cJSON_IsString(jrun) && jrun->valuestring && jrun->valuestring[0])
   {
      opts.cancel_requested = roundtable_run_cancel_requested;
      opts.cancel_ctx = jrun->valuestring;
   }
   normalized_roundtable_brief_t brief;
   char brief_err[128];
   brief_err[0] = '\0';
   if (normalize_roundtable_brief(req, &brief, brief_err, sizeof(brief_err)) != 0)
      return server_send_error(conn, brief_err[0] ? brief_err : "invalid brief", NULL);
   opts.brief = brief.rendered;
   opts.brief_truncated = brief.truncated;
   opts.questions = brief.question_ptrs;
   opts.question_count = brief.question_count;

   cJSON *jmode = cJSON_GetObjectItemCaseSensitive(req, "mode");
   if (cJSON_IsString(jmode) && strcmp(jmode->valuestring, "review") == 0)
      opts.mode = ROUNDTABLE_REVIEW;
   cJSON *jturns = cJSON_GetObjectItemCaseSensitive(req, "turns");
   if (cJSON_IsString(jturns) && strcmp(jturns->valuestring, "sequential") == 0)
      opts.turns = ROUNDTABLE_SEQUENTIAL;
   else if (cJSON_IsString(jturns) && strcmp(jturns->valuestring, "parallel") == 0)
      opts.turns = ROUNDTABLE_PARALLEL;
   cJSON *jrounds = cJSON_GetObjectItemCaseSensitive(req, "rounds");
   if (cJSON_IsNumber(jrounds) && jrounds->valuedouble > 0)
   {
      opts.max_rounds = (int)jrounds->valuedouble;
      if (opts.max_rounds > ROUNDTABLE_MAX_ROUNDS_REQUEST)
         opts.max_rounds = ROUNDTABLE_MAX_ROUNDS_REQUEST;
   }
   cJSON *japply = cJSON_GetObjectItemCaseSensitive(req, "apply");
   if (cJSON_IsBool(japply))
      opts.apply_review = cJSON_IsTrue(japply) ? 1 : 0;

   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   if (agent_load_config(&acfg) != 0)
   {
      free(brief.rendered);
      return server_send_error(conn, "could not load agents.json", NULL);
   }

   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, prompt, &opts, &result);
   if (rc != 0)
   {
      free(brief.rendered);
      return server_send_error(
          conn, "roundtable run failed (check ensemble.enabled / ensemble.reference_models)", NULL);
   }

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "artifact", result.artifact ? result.artifact : "");
   cJSON_AddNumberToObject(resp, "rounds_run", result.rounds_run);
   cJSON_AddBoolToObject(resp, "converged", result.converged ? 1 : 0);
   cJSON_AddBoolToObject(resp, "degraded", result.degraded ? 1 : 0);
   cJSON_AddBoolToObject(resp, "truncated", result.truncated ? 1 : 0);
   cJSON_AddBoolToObject(resp, "cost_capped", result.cost_capped ? 1 : 0);
   cJSON_AddBoolToObject(resp, "deadline_hit", result.deadline_hit ? 1 : 0);
   cJSON_AddBoolToObject(resp, "cancelled", result.cancelled ? 1 : 0);
   cJSON_AddNumberToObject(resp, "best_round", result.best_round);
   cJSON_AddNumberToObject(resp, "items_round", result.items_round);
   cJSON_AddNumberToObject(resp, "artifact_round", result.artifact_round);
   cJSON_AddNumberToObject(resp, "cost_usd", result.cost_usd);
   add_roundtable_arrays(resp, &result);
   delegate_roundtable_result_free(&result);
   free(brief.rendered);
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
#include "server_compute_episodes.inc"
