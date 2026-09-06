/* server.c: aimee-server core -- event loop, connection handling, method dispatch */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_internal.h"
#include "aimee.h"
#include "harness_memory_scope.h" /* hmem_scope_for_client */
#include "hook_session_token.h"
#include "json_fluent.h" /* jo_ok */
#include "primary_cli_ingestor.h"
#include "server.h"
#include "server_mcp_internal.h" /* mcp_tool_register_native_surface */
#include "kb_client.h"           /* request-local memory scope context */
#include <aimee/audit/obs_bus.h>
#include <aimee/tools/agent_tools.h> /* agent_tools_set_git_write_provider / _set_shell_git_gate */
#include "modules/git/git_cred_inject.h" /* git_cred_forge_configured — no aimee route, no restriction */
#include "modules/git/mcp_git.h" /* mcp_git_run_tool — the native surface's git-write impl */
#include "wfe_native_gate.h"     /* wfe_shell_invokes_git — the shell-git classifier */
#include "turn_registry.h"
#include "server_http.h"
#include "server_tls.h" /* server_http_api_status_report */
#include "server_mgmt_status.h"
#include "server_mgmt_jwks_cache.h"
#include "kb_client_mtls.h"
#include "config.h" /* config accessors for api.status, api.enable */
#include <aimee/delegates/delegate_backend_docker.h>
#include "modules/workspace/workspace_provider.h" /* the shared provider: probe docker for the sandbox posture */
#include "modules/workspace/workspace_turn.h" /* the ONE workspace bound, shared with the delegate turn */
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
#include "module_stage_adapters.h"
#include "trigger_scheduler.h"
#include "server_trigger.h"
#include "server_cron.h"
#include "server_pipeline.h" /* roundtable authoring pipeline (pipeline.*) */
#include "commands.h"
#include "agent.h"
#include "agent_exec.h"      /* agent_audit_async_flush — drain audit queue at shutdown */
#include "webuser_editor.h"  /* webuser_editor_shutdown — reap editors at shutdown (WP-I) */
#include "agent_admission.h" /* agent_admission_agent_active — route capacity probe */
#include "agent_config.h"
#include "provider_catalog.h"
#include <aimee/delegates/delegate_credentials.h>
#include <aimee/delegates/delegate_sandbox_image.h>
#include "model_registry.h"
#include "model_provider.h"
#include "db1_client/db1.h"
#include "db1_client/user_memory.h"
#include "token_audit.h"
#include "dashboard.h"
#include "log.h"
#include "modules/workspace/workspace_scope.h" /* ws_scope_openat2_available — webuser surface gate */
#include "ws_registry.h"                       /* ws_reg_rebuild at startup */
#include "hud.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "util.h"
#include <aimee/workspace/workspace.h>
#include "worktree_gc.h"
#include "modules/git/git_verify.h"
#include "toolset.h"
#include "cJSON.h"
#include "s2_native_gate_hook.h" /* S2 native-tool gate (server-side, tracks 2+3) */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
/* Defined in server_main.c; set by the SIGHUP handler, observed by the main loop (P1b). */
extern volatile sig_atomic_t g_config_reload_requested;
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
extern int hooks_ensure_cwd_worktree(session_state_t *state, const char *sid, const char *cwd);

typedef int (*server_method_handler_t)(server_ctx_t *, server_conn_t *, cJSON *);
typedef struct
{
   const char *method;
   server_method_handler_t handler;
} server_method_dispatch_t;
#define SERVER_REQUEST_POOL_MAX_THREADS   4
#define SERVER_ORCHESTRATION_POOL_THREADS 16
static const server_method_dispatch_t server_dispatch_table[];
int handle_toolset_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_toolset_resolve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_toolset_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
static int64_t monotonic_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int server_request_pool_thread_count(void)
{
   return SERVER_REQUEST_POOL_MAX_THREADS;
}

static void server_request_pool_shutdown(server_ctx_t *ctx)
{
   if (!ctx || !ctx->request_pool_initialized)
      return;
   compute_pool_unregister_secondary(&ctx->request_pool);
   compute_pool_shutdown(&ctx->request_pool);
   ctx->request_pool_initialized = 0;
}

static void server_orchestration_pool_shutdown(server_ctx_t *ctx)
{
   if (!ctx || !ctx->orchestration_pool_initialized)
      return;
   compute_pool_unregister_secondary(&ctx->orchestration_pool);
   compute_pool_shutdown(&ctx->orchestration_pool);
   ctx->orchestration_pool_initialized = 0;
}

static void server_orchestration_pool_close(server_ctx_t *ctx)
{
   if (ctx && ctx->orchestration_pool_initialized)
      compute_pool_close(&ctx->orchestration_pool);
}

int server_compute_budget_acquire(server_ctx_t *ctx)
{
   pthread_mutex_lock(&ctx->compute_budget_mutex);
   while (ctx->compute_budget_available <= 0)
      pthread_cond_wait(&ctx->compute_budget_cond, &ctx->compute_budget_mutex);

   /* Consume one server compute slot per queued job. Model/provider
    * concurrency limits decide how many delegates may run in parallel. */
   ctx->compute_budget_available--;
   pthread_mutex_unlock(&ctx->compute_budget_mutex);

   return 1;
}

void server_compute_budget_release(server_ctx_t *ctx, int granted)
{
   if (granted <= 0)
      return;

   pthread_mutex_lock(&ctx->compute_budget_mutex);
   ctx->compute_budget_available += granted;
   if (ctx->compute_budget_available > ctx->compute_budget_total)
      ctx->compute_budget_available = ctx->compute_budget_total;
   pthread_cond_broadcast(&ctx->compute_budget_cond);
   pthread_mutex_unlock(&ctx->compute_budget_mutex);
}

/* Update event loop registration: IN always, OUT when there's pending data.
 *
 * Only meaningful for connections owned by the epoll accept loop. The /v1 HTTP
 * workers (server_http.c) run their own blocking per-connection loop and build
 * a memset-zeroed server_conn_t whose evloop is NULL and whose fd was never
 * registered with any epoll set; for them this is a no-op (dereferencing a NULL
 * evloop here previously crashed the whole server on every buffered response). */
static void conn_update_events(server_conn_t *conn)
{
   if (!conn->evloop)
      return;
   uint32_t events = PLAT_EV_IN;
   if (conn->write_len > 0)
      events |= PLAT_EV_OUT;
   platform_evloop_mod(conn->evloop, conn->fd, events);
}

/* Flush pending output buffer to kernel. Returns 0 on success/partial, -1 on error. */
static int conn_flush(server_conn_t *conn)
{
   while (conn->write_len > 0)
   {
      ssize_t n = write(conn->fd, conn->write_buf + conn->write_pos, conn->write_len);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; /* can't write more right now */
         return -1;
      }
      conn->write_pos += (size_t)n;
      conn->write_len -= (size_t)n;
   }

   /* All flushed: reset buffer and clear deadline */
   conn->write_pos = 0;
   conn->write_len = 0;
   conn->write_deadline_ms = 0;
   conn_update_events(conn);
   return 0;
}

/* Queue data for sending. Tries to write immediately; buffers remainder. */
static int conn_send(server_conn_t *conn, const char *data, size_t len)
{
   /* Try to flush any existing pending data first */
   if (conn->write_len > 0)
   {
      if (conn_flush(conn) < 0)
         return -1;
   }

   /* If buffer is empty, try writing directly to avoid buffering */
   if (conn->write_len == 0)
   {
      while (len > 0)
      {
         ssize_t n = write(conn->fd, data, len);
         if (n < 0)
         {
            if (errno == EINTR)
               continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
               break; /* buffer the rest */
            return -1;
         }
         data += n;
         len -= (size_t)n;
      }

      if (len == 0)
         return 0; /* all sent immediately */
   }

   /* Buffer remaining data */
   /* Compact buffer if needed to make room at the end */
   if (conn->write_pos > 0 && conn->write_pos + conn->write_len + len > SERVER_WRITE_BUF_SIZE)
   {
      memmove(conn->write_buf, conn->write_buf + conn->write_pos, conn->write_len);
      conn->write_pos = 0;
   }

   /* Check if there's enough room */
   if (conn->write_pos + conn->write_len + len > SERVER_WRITE_BUF_SIZE)
      return -1; /* buffer overflow: client too slow */

   memcpy(conn->write_buf + conn->write_pos + conn->write_len, data, len);
   conn->write_len += len;

   /* Set write deadline if this is the first pending data */
   if (conn->write_deadline_ms == 0)
      conn->write_deadline_ms = monotonic_ms() + CONN_WRITE_DEADLINE_MS;

   conn_update_events(conn);
   return 0;
}

static int conn_send_blocking(server_conn_t *conn, const char *data, size_t len)
{
   if (!conn || conn->fd < 0)
      return -1;
   if (conn->write_len > 0 && conn_flush(conn) < 0)
      return -1;
   if (conn->write_len > 0)
      return -1;

   int64_t deadline = monotonic_ms() + CONN_WRITE_DEADLINE_MS;
   while (len > 0)
   {
      ssize_t n = write(conn->fd, data, len);
      if (n > 0)
      {
         data += n;
         len -= (size_t)n;
         continue;
      }
      if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
         return -1;
      if (errno == EINTR)
         continue;
      int64_t now = monotonic_ms();
      if (now >= deadline)
         return -1;
      struct pollfd pfd = {.fd = conn->fd, .events = POLLOUT};
      if (poll(&pfd, 1, (int)(deadline - now)) <= 0)
         return -1;
   }
   return 0;
}

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   char *json_str = cJSON_PrintUnformatted(resp);
   if (!json_str)
      return -1;

   size_t len = strlen(json_str);
   int lock_conn = conn && conn->in_use;
   if (lock_conn)
      pthread_mutex_lock(&conn->mutex);

   int rc = -1;
   if (conn && (!lock_conn || (!conn->closing && conn->fd >= 0)))
   {
      if (len + 1 > SERVER_WRITE_BUF_SIZE)
         rc = conn_send_blocking(conn, json_str, len);
      else
         rc = conn_send(conn, json_str, len);
   }
   free(json_str);

   /* Send newline delimiter */
   if (rc == 0)
   {
      if (len + 1 > SERVER_WRITE_BUF_SIZE)
         rc = conn_send_blocking(conn, "\n", 1);
      else
         rc = conn_send(conn, "\n", 1);
   }

   if (lock_conn)
      pthread_mutex_unlock(&conn->mutex);
   return rc;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   return server_send_error_kind(conn, NULL, message, request_id);
}

/* --- Method handlers --- */

static int handle_server_info(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "protocol_version", SERVER_PROTOCOL_VERSION);
   cJSON_AddStringToObject(resp, "server_version", AIMEE_VERSION);
   cJSON_AddStringToObject(resp, "build_id", AIMEE_BUILD_ID);
   cJSON_AddNumberToObject(resp, "commit_time", (double)AIMEE_GIT_COMMIT_TIME);
   /* Expose the server's own executable path so the client can tell "my server,
    * out of date — restart" (matching path) from "server lives elsewhere — leave
    * it alone", preventing dev/installed CLIs from SIGTERMing each other's
    * servers. */
   {
      char exe_path[1024];
      ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
      if (len > 0)
      {
         exe_path[len] = '\0';
         cJSON_AddStringToObject(resp, "executable_path", exe_path);
      }
   }
   cJSON *methods = cJSON_CreateArray();
   if (methods)
   {
      for (int i = 0; server_dispatch_table[i].method; i++)
         cJSON_AddItemToArray(methods, cJSON_CreateString(server_dispatch_table[i].method));
      cJSON_AddItemToObject(resp, "methods", methods);
   }
   return server_send_ok(conn, resp);
}

static int handle_server_health(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)req;
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "uptime", (double)(time(NULL) - ctx->start_time));
   /* Probe DB1 directly; module registry state survives death for about 37s. */
   cJSON_AddStringToObject(resp, "state", db1_store_probe() ? "ok" : "unavailable");
   cJSON_AddNumberToObject(resp, "connections", ctx->conn_count);
   obs_bus_capture_health_t capture;
   obs_bus_capture_health(&capture);
   cJSON_AddBoolToObject(resp, "capture_ok", capture.capture_ok);
   cJSON_AddStringToObject(resp, "capture_reason", capture.reason);
   cJSON_AddStringToObject(resp, "capture_session_id", capture.session_id);
   cJSON_AddNumberToObject(resp, "capture_last_seq", (double)capture.last_seq);
   server_health_add_kb(resp); /* kb block — see server_api_status.c */
   return server_send_ok(conn, resp);
}
/* worktree.gc: remove abandoned session worktrees under the operator's git_root.
 * Server-side because the GC primitive lives in workspace.c (already linked into
 * the daemon); the CLI just sends client_cwd and renders the report. */
static int handle_worktree_gc(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "client_cwd");
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : NULL;
   if (!cwd || !cwd[0])
      return server_send_error(conn, "worktree.gc: missing client_cwd", NULL);

   char git_root[MAX_PATH_LEN];
   if (git_repo_root(cwd, git_root, sizeof(git_root)) != 0)
      return server_send_error(conn, "worktree.gc: client_cwd is not in a git repo", NULL);

   worktree_gc_options_t opts;
   worktree_gc_options_init(&opts);
   cJSON *jdays = cJSON_GetObjectItemCaseSensitive(req, "max_age_days");
   if (cJSON_IsNumber(jdays) && (int)jdays->valuedouble > 0)
      opts.max_age_days = (int)jdays->valuedouble;
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "force")))
      opts.force = 1;
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "dry_run")))
      opts.dry_run = 1;

   worktree_gc_candidate_t cands[WORKTREE_GC_MAX_CANDIDATES];
   int n = worktree_gc_scan(git_root, &opts, cands, WORKTREE_GC_MAX_CANDIDATES);
   if (n < 0)
      return server_send_error(conn, "worktree.gc: scan failed", NULL);
   int removed = worktree_gc_apply(git_root, cands, n, &opts);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "git_root", git_root);
   cJSON_AddNumberToObject(resp, "scanned", n);
   cJSON_AddNumberToObject(resp, "removed", removed);
   cJSON_AddNumberToObject(resp, "max_age_days", opts.max_age_days);
   cJSON_AddBoolToObject(resp, "dry_run", opts.dry_run ? 1 : 0);
   cJSON_AddBoolToObject(resp, "force", opts.force ? 1 : 0);
   cJSON *jcands = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *c = cJSON_CreateObject();
      cJSON_AddStringToObject(c, "path", cands[i].path);
      cJSON_AddStringToObject(c, "branch", cands[i].branch);
      cJSON_AddStringToObject(c, "reason", cands[i].reason);
      cJSON_AddNumberToObject(c, "last_activity", (double)cands[i].last_activity);
      cJSON_AddNumberToObject(c, "commits_ahead", cands[i].commits_ahead);
      cJSON_AddBoolToObject(c, "has_uncommitted", cands[i].has_uncommitted ? 1 : 0);
      cJSON_AddBoolToObject(c, "eligible", cands[i].eligible ? 1 : 0);
      cJSON_AddItemToArray(jcands, c);
   }
   cJSON_AddItemToObject(resp, "candidates", jcands);
   return server_send_ok(conn, resp);
}

/* Return the registered delegate-execution backends as a JSON array
 * under `backends`. Builds on the iter-44 helper so the registry
 * inspector code stays in delegate_backend.c. Used by the future
 * `aimee delegate-backend list` CLI command (separate iter); also
 * useful for ops via raw RPC. */
static int handle_delegate_backend_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   cJSON *resp = cJSON_CreateObject();
   char *list_json = delegate_backend_list_json();
   cJSON *arr = list_json ? cJSON_Parse(list_json) : NULL;
   free(list_json);
   if (!arr)
      arr = cJSON_CreateArray(); /* never embed NULL — empty array on alloc fail */
   cJSON_AddItemToObject(resp, "backends", arr);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

/* delegate.sandbox_list: enumerate build-from-spec images (tag prefix aimee-sbx:)
 * with created-time and in-use flags. Server-side because the docker daemon is
 * server-side; the CLI just renders the array. */
static int handle_delegate_sandbox_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = delegate_sandbox_images_json();
   if (!json)
      return server_send_error(conn, "delegate.sandbox_list: docker daemon unreachable", NULL);
   cJSON *arr = cJSON_Parse(json);
   free(json);
   if (!arr)
      arr = cJSON_CreateArray(); /* never embed NULL */
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddItemToObject(resp, "images", arr);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

/* delegate.sandbox_gc: prune build-from-spec images that are not referenced by any
 * container and older than max_age_days (default 7), always keeping the `keep` most
 * recent (default 3). dry_run reports what would go without removing. */
static int handle_delegate_sandbox_gc(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   long max_age_days = 7;
   cJSON *jdays = cJSON_GetObjectItemCaseSensitive(req, "max_age_days");
   if (cJSON_IsNumber(jdays) && jdays->valuedouble >= 0)
      max_age_days = (long)jdays->valuedouble;
   int keep = 3;
   cJSON *jkeep = cJSON_GetObjectItemCaseSensitive(req, "keep");
   if (cJSON_IsNumber(jkeep) && jkeep->valuedouble >= 0)
      keep = (int)jkeep->valuedouble;
   int dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "dry_run")) ? 1 : 0;

   char *report = NULL;
   if (delegate_sandbox_gc(max_age_days * 86400L, keep, dry_run, &report) != 0)
   {
      free(report);
      return server_send_error(conn, "delegate.sandbox_gc: docker daemon unreachable", NULL);
   }
   cJSON *resp = report ? cJSON_Parse(report) : NULL;
   free(report);
   if (!resp)
      resp = cJSON_CreateObject(); /* report carries removed/kept/dry_run/images */
   cJSON_AddNumberToObject(resp, "max_age_days", (double)max_age_days);
   cJSON_AddNumberToObject(resp, "keep", keep);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

/* Drive one backend through acquire/exec/release. Operator-facing —
 * useful for smoke-testing the registry end-to-end without touching
 * the legacy delegate flow. Request fields:
 *   backend       (required) — registry key, e.g. "local"
 *   task_id       (required) — workspace identifier
 *   command       (required) — bash command line for exec()
 *   image         (optional) — docker image hint
 *   workspace     (optional) — host dir to expose AS the workspace (a checkout or
 *                              a linked worktree); default = the backend's scratch
 *   workspace_read_only (optional bool) — mount `workspace` :ro
 *   host          (optional) — ssh target
 *   no_hibernate  (optional bool) — release with hibernate=0 if true
 * Response fields:
 *   status, exit_code, latency_ms, stdout, stderr (truncated to 64K)
 *   plus error/message on failure paths. */
static int handle_delegate_backend_exec(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jb = cJSON_GetObjectItemCaseSensitive(req, "backend");
   cJSON *jt = cJSON_GetObjectItemCaseSensitive(req, "task_id");
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "command");
   if (!cJSON_IsString(jb) || !jb->valuestring[0] || !cJSON_IsString(jt) || !jt->valuestring[0] ||
       !cJSON_IsString(jc) || !jc->valuestring[0])
      return server_send_error(conn, "backend, task_id, command are required strings", NULL);

   delegate_backend_t *b = delegate_backend_lookup(jb->valuestring);
   if (!b)
      return server_send_error(conn, "unknown backend", jb->valuestring);

   delegate_backend_config_t cfg = {0};
   cfg.hibernate_on_exit = 1;
   cJSON *jimg = cJSON_GetObjectItemCaseSensitive(req, "image");
   if (cJSON_IsString(jimg))
      cfg.image = jimg->valuestring;
   cJSON *jhost = cJSON_GetObjectItemCaseSensitive(req, "host");
   if (cJSON_IsString(jhost))
      cfg.host = jhost->valuestring;
   /* The workspace to expose to the container, and whether it is the delegate's to
    * change. This RPC exists to smoke-test the registry end-to-end without the
    * legacy delegate flow, and the mount is the part that most needs a real daemon
    * to believe: a linked worktree's gitlink only resolves if the repo is mounted
    * at its own absolute path, which no unit test can prove. */
   cJSON *jws = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   char ws_authorized[MAX_PATH_LEN] = "";
   if (cJSON_IsString(jws) && jws->valuestring[0])
   {
      /* Through the SAME bound the delegate turn uses. This RPC takes a path from a
       * request, so without it any caller who can reach the registry could name any
       * host directory and have it bind-mounted into a container. The seam having
       * the check is not enough — a second entrance needs the same lock, and the
       * helper exists precisely so there is one lock rather than two copies. */
      if (!workspace_turn_workspace_authorized(jws->valuestring, ws_authorized,
                                               sizeof(ws_authorized)))
         return server_send_error(conn, "workspace not inside a registered workspace root",
                                  jws->valuestring);
      cfg.workspace = ws_authorized;
   }
   cJSON *jro = cJSON_GetObjectItemCaseSensitive(req, "workspace_read_only");
   if (cJSON_IsBool(jro) && cJSON_IsTrue(jro))
      cfg.workspace_read_only = 1;
   cJSON *jnh = cJSON_GetObjectItemCaseSensitive(req, "no_hibernate");
   if (cJSON_IsBool(jnh) && cJSON_IsTrue(jnh))
      cfg.hibernate_on_exit = 0;

   void *state = NULL;
   if (b->acquire(b, jt->valuestring, &cfg, &state) != 0)
      return server_send_error(conn, "backend acquire failed", jb->valuestring);

   enum
   {
      BUF_CAP = 64 * 1024
   };
   char *out = calloc(1, BUF_CAP);
   char *err = calloc(1, BUF_CAP);
   if (!out || !err)
   {
      free(out);
      free(err);
      b->release(b, state, cfg.hibernate_on_exit);
      return server_send_error(conn, "out of memory", NULL);
   }
   delegate_exec_result_t r;
   memset(&r, 0, sizeof(r));
   r.stdout_buf = out;
   r.stdout_cap = BUF_CAP;
   r.stderr_buf = err;
   r.stderr_cap = BUF_CAP;
   int erc = b->exec(b, state, jc->valuestring, 60000, &r);

   cJSON *resp = cJSON_CreateObject();
   if (erc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "backend exec failed");
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddNumberToObject(resp, "exit_code", r.exit_code);
      cJSON_AddNumberToObject(resp, "latency_ms", r.latency_ms);
      cJSON_AddStringToObject(resp, "stdout", out);
      cJSON_AddStringToObject(resp, "stderr", err);
   }
   b->release(b, state, cfg.hibernate_on_exit);
   free(out);
   free(err);
   return server_send_ok(conn, resp);
}

static int server_sibling_kb_path(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   if (platform_get_exe_path(out, out_len) != 0)
      return -1;
   char *slash = strrchr(out, '/');
   char *backslash = strrchr(out, '\\');
   if (!slash || (backslash && backslash > slash))
      slash = backslash;
   if (!slash)
      return -1;

   int wants_exe = strstr(slash + 1, ".exe") != NULL;
   snprintf(slash + 1, out_len - (size_t)(slash - out) - 1, "aimee-kb%s", wants_exe ? ".exe" : "");
   return 0;
}

static cJSON *server_run_kb_bootstrap(void)
{
   char kb_path[MAX_PATH_LEN];
   if (server_sibling_kb_path(kb_path, sizeof(kb_path)) != 0)
      return NULL;

   char *quoted = shell_quote(kb_path);
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "%s --bootstrap-db2 --json", quoted);
   free(quoted);

   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_capture(cmd, &out, &out_len, 300000);
   (void)out_len;
   if (!out)
      return NULL;

   const char *json_start = strchr(out, '{');
   cJSON *parsed = json_start ? cJSON_Parse(json_start) : NULL;
   if (!parsed)
   {
      parsed = cJSON_CreateObject();
      cJSON_AddStringToObject(parsed, "status", rc == 0 ? "ok" : "error");
      cJSON_AddStringToObject(parsed, "message", out);
   }
   cJSON_AddNumberToObject(parsed, "exit_code", rc);
   free(out);
   return parsed;
}

/* handle_launch_run: typed RPC for the no-arg `aimee` invocation. The
 * client uses the response (session_id, provider, worktree_cwd) to chdir
 * and exec the configured provider CLI. */
static int handle_launch_run(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *cwd = NULL;
   cJSON *jcwd_in = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   if (cJSON_IsString(jcwd_in) && jcwd_in->valuestring[0])
      cwd = jcwd_in->valuestring;

   /* Fresh session ID per launch; never inherit a stale session-ppid file. */
   char sid[33];
   {
      unsigned char rnd[16];
      if (platform_random_bytes(rnd, sizeof(rnd)) != 0)
         return server_send_error(conn, "secure entropy unavailable; session not created", NULL);
      for (size_t i = 0; i < sizeof(rnd); i++)
         snprintf(sid + i * 2, sizeof(sid) - i * 2, "%02x", rnd[i]);
   }

   /* Copied out: held across the worktree/session work below. */
   char provider_buf[CONFIG_COPY_MAX];
   config_provider_copy(provider_buf, sizeof(provider_buf));
   const char *provider = provider_buf[0] ? provider_buf : "claude";
   int builtin = 1;

   /* Worktree mappings persist in DB1; load (or initialize) per sid. */
   session_state_t state;
   session_state_load(&state, sid);
   hooks_ensure_cwd_worktree(&state, sid, cwd);

   for (int i = 0; i < state.worktree_count; i++)
      worktree_create_sibling(state.worktrees[i].git_root, sid, NULL);

   char target_dir[MAX_PATH_LEN] = "";
   if (cwd && state.worktree_count > 0)
   {
      const char *wt = worktree_for_cwd(&state, cwd);
      if (wt)
      {
         for (int i = 0; i < state.worktree_count; i++)
         {
            size_t rlen = strlen(state.worktrees[i].git_root);
            if (strncmp(cwd, state.worktrees[i].git_root, rlen) == 0 &&
                (cwd[rlen] == '/' || cwd[rlen] == '\0'))
            {
               const char *suffix = cwd + rlen;
               snprintf(target_dir, sizeof(target_dir), "%s%s", state.worktrees[i].worktree_path,
                        suffix);
               state.dirty = 1;
               session_state_save(&state, sid);
               break;
            }
         }
      }
      else
      {
         /* CWD is a parent of a tracked git root; map onto the worktree path. */
         size_t cwd_len = strlen(cwd);
         for (int i = 0; i < state.worktree_count; i++)
         {
            const char *gr = state.worktrees[i].git_root;
            if (strncmp(gr, cwd, cwd_len) == 0 && (gr[cwd_len] == '/' || gr[cwd_len] == '\0'))
            {
               snprintf(target_dir, sizeof(target_dir), "%s", state.worktrees[i].worktree_path);
               state.dirty = 1;
               session_state_save(&state, sid);
               break;
            }
         }
      }
   }
   if (state.dirty)
      session_state_save(&state, sid);

   cJSON *launch_resp = jo_ok();
   cJSON_AddStringToObject(launch_resp, "session_id", sid);
   cJSON_AddStringToObject(launch_resp, "provider", provider);
   if (strcmp(provider, "claude") == 0 && config_claude_model()[0])
      cJSON_AddStringToObject(launch_resp, "model", config_claude_model());
   else if ((strcmp(provider, "codex") == 0 || strcmp(provider, "codex-oauth") == 0 ||
             strcmp(provider, "chatgpt") == 0) &&
            config_codex_model()[0])
      cJSON_AddStringToObject(launch_resp, "model", config_codex_model());
   cJSON_AddBoolToObject(launch_resp, "builtin", builtin);
   if (config_autonomous())
      cJSON_AddBoolToObject(launch_resp, "autonomous", 1);
   if (target_dir[0])
      cJSON_AddStringToObject(launch_resp, "worktree_cwd", target_dir);

   return server_send_ok(conn, launch_resp);
}

static int handle_init_run(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *cwd = NULL;
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
      cwd = jcwd->valuestring;

   cJSON *resp = jo_ok();
   cJSON_AddBoolToObject(resp, "local_ready", db1_store_ready() ? 1 : 0);
   cJSON_AddBoolToObject(resp, "db1_ready", db1_store_ready() ? 1 : 0);

   if (cwd)
   {
      stack_info_t stacks[MAX_DETECTED_STACKS];
      int stack_count = detect_project_stacks(cwd, stacks, MAX_DETECTED_STACKS);
      int rules_rc = write_aimee_rules_file(cwd, stacks, stack_count);
      cJSON_AddStringToObject(resp, "cwd", cwd);
      cJSON_AddNumberToObject(resp, "stack_count", stack_count);
      cJSON_AddBoolToObject(resp, "rules_generated", rules_rc == 0 ? 1 : 0);
      cJSON_AddBoolToObject(resp, "rules_already_exists", rules_rc == 1 ? 1 : 0);
   }

   cJSON *kb = server_run_kb_bootstrap();
   if (kb)
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(kb, "status");
      int knowledge_ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
      cJSON_AddBoolToObject(resp, "knowledge_ready", knowledge_ok ? 1 : 0);
      cJSON_AddItemToObject(resp, "knowledge", kb);
   }
   else
   {
      cJSON_AddBoolToObject(resp, "knowledge_ready", 0);
      cJSON *knowledge = cJSON_AddObjectToObject(resp, "knowledge");
      cJSON_AddStringToObject(knowledge, "status", "error");
      cJSON_AddStringToObject(knowledge, "message", "aimee-kb bootstrap helper unavailable");
   }

   return server_send_ok(conn, resp);
}

static int handle_hud_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   if (!db1_store_ready())
      return server_send_error(conn, "server storage unavailable", NULL);
   hud_status_t hs;
   if (hud_gather(&hs) != 0)
      return server_send_error(conn, "status aggregation failed", NULL);
   char *json_str = hud_json(&hs);
   if (!json_str)
      return server_send_error(conn, "json serialization failed", NULL);
   cJSON *parsed = cJSON_Parse(json_str);
   free(json_str);
   if (!parsed)
      return server_send_error(conn, "json parse failed", NULL);
   return server_send_ok(conn, parsed);
}

static void server_hook_principal(const server_conn_t *conn, char *out, size_t cap)
{
   if (conn->vault_principal[0])
      snprintf(out, cap, "%s", conn->vault_principal);
   else
      snprintf(out, cap, "transport:%d:uid:%u", (int)conn->attested_transport,
               (unsigned)conn->peer_uid);
}

/* 1 = trusted, 0 = continue without identity-derived privileges, -1 = hardened
 * refusal. AIMEE_HOOK_IDENTITY_MODE is intentionally server-only operational
 * policy: observe and enforce both refuse attribution, while hardened refuses
 * the entire hook before any stateful policy consumer runs. */
static int server_hook_identity(server_conn_t *conn, cJSON *req, const char *sid,
                                const char **trusted_client)
{
   *trusted_client = NULL;
   const char *client =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "harness_client"));
   const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "hook_token"));
   if (!client || !client[0])
      return 0;
   char principal[128];
   server_hook_principal(conn, principal, sizeof(principal));
   if (hook_session_token_verify(sid, client, principal, token))
   {
      *trusted_client = client;
      return 1;
   }
   const char *mode = getenv("AIMEE_HOOK_IDENTITY_MODE");
   LOG_WARN("hook_identity", "untrusted hook session=%s client=%s mode=%s", sid, client,
            mode && mode[0] ? mode : "enforce");
   return mode && !strcmp(mode, "hardened") ? -1 : 0;
}

/* S2 pre-delivery native-tool externalization gate (server side; tracks 2+3). This
 * is the REAL enforcement point -- `aimee hooks pre` forwards to hooks.pre, so the
 * gate must run here (the CLI cmd_hooks copy is only a server-unreachable fallback).
 * Returns 2 to DENY (fills msg) or 0 to allow. Mirrors handle_hooks_pre's memory-
 * interception deny. Honest scope + fail policy: see wfe_native_gate.h / cmd_hooks.c. */
static int handle_hooks_pre(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jtn = cJSON_GetObjectItemCaseSensitive(req, "tool_name");
   cJSON *jti = cJSON_GetObjectItemCaseSensitive(req, "tool_input");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jrid = cJSON_GetObjectItemCaseSensitive(req, "request_id");

   const char *request_id = cJSON_IsString(jrid) ? jrid->valuestring : NULL;

   if (!cJSON_IsString(jtn))
      return server_send_error(conn, "missing tool_name", request_id);

   const char *tool_name = jtn->valuestring;
   char *ti_heap = NULL;
   const char *tool_input = "{}";

   if (cJSON_IsString(jti))
      tool_input = jti->valuestring;
   else if (cJSON_IsObject(jti) || cJSON_IsArray(jti))
   {
      ti_heap = cJSON_PrintUnformatted(jti);
      tool_input = ti_heap;
   }

   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : "unknown";
   if (!is_safe_id(sid))
      return server_send_error(conn, "invalid session_id (must be alphanumeric/dash/underscore)",
                               request_id);

   const char *trusted_client = NULL;
   int hook_identity = server_hook_identity(conn, req, sid, &trusted_client);
   if (hook_identity < 0)
      return hook_send_blocked(conn, "Unauthenticated or mismatched session hook identity.",
                               request_id);

   session_state_t state;
   session_state_load(&state, sid);

   /* Memory interception: redirect an agent's local memory-file write into the
    * central store BEFORE the generic guardrails see it (rc==2 -> client deny). */
   {
      char mr_msg[1024] = "";
      if (trusted_client && server_memory_intercept(tool_name, tool_input, cwd, req, trusted_client,
                                                    mr_msg, sizeof(mr_msg)) == 2)
      {
         free(ti_heap);
         return hook_send_blocked(conn, mr_msg, request_id);
      }
   }

   /* S2 native-tool externalization gate (tracks 2+3): sends the block on deny. */
   int s2rc = s2_native_gate_hook_pre(conn, sid, tool_name, tool_input, request_id);
   if (s2rc >= 0)
   {
      free(ti_heap);
      return s2rc;
   }

   /* Run guardrail check */
   char msg[1024] = "";
   /* A hook describes the CLIENT's filesystem. Bind the same registered runner
    * as tool execution, including the legacy run_cmd probes used by worktree,
    * branch and verify guards. Binding only the file-tool provider leaves those
    * probes on the server and makes every remote push unresolvable.
    * Worktree creation/routing belongs to the thin client for detached roots. */
   cJSON *input = cJSON_Parse(tool_input);
   const char *target_cwd = cwd;
   static const char *cwd_keys[] = {"workdir", "cwd", "working_dir", "working_directory", NULL};
   for (int i = 0; cwd_keys[i]; i++)
   {
      const char *value =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, cwd_keys[i]));
      if (value && value[0])
      {
         target_cwd = value;
         break;
      }
   }
   char saved_cwd[MAX_PATH_LEN];
   snprintf(saved_cwd, sizeof(saved_cwd), "%s", run_cmd_get_cwd() ? run_cmd_get_cwd() : "");
   int bound = workspace_turn_bind_active(target_cwd);
   const workspace_provider_t *provider = workspace_provider_active();
   int detached = bound && provider->kind == WS_PROVIDER_DETACHED;
   if (detached)
      run_cmd_set_cwd(target_cwd);
   else
      hooks_ensure_cwd_worktree(&state, sid, cwd);
   int rc = pre_tool_check(tool_name, tool_input, &state, config_guardrail_mode(), cwd, msg,
                           sizeof(msg));
   run_cmd_set_cwd(saved_cwd[0] ? saved_cwd : NULL);
   workspace_turn_unbind_active();
   cJSON_Delete(input);

   session_state_save(&state, sid);

   /* Sub-agent interception (enforce-delegate-only): the primary agent must not
    * spawn its OWN sub-agents. pre_tool_check already blocks Task/Agent/spawn_agent
    * (rc==2); if aimee has usable delegates, auto-launch a delegate from the call
    * and point the model at the job. The call stays blocked — only the message
    * changes (a PreToolUse hook cannot return the delegate's result inline). */
   if (rc == 2 && strcmp(guardrails_canonical_tool_name(tool_name), "Subagent") == 0 &&
       agent_any_delegate_available())
   {
      char prompt[8192] = "";
      cJSON *ti = cJSON_Parse(tool_input);
      if (ti)
      {
         cJSON *jp = cJSON_GetObjectItemCaseSensitive(ti, "prompt");
         cJSON *jd = cJSON_GetObjectItemCaseSensitive(ti, "description");
         const char *p = (cJSON_IsString(jp) && jp->valuestring[0]) ? jp->valuestring : NULL;
         const char *d = (cJSON_IsString(jd) && jd->valuestring[0]) ? jd->valuestring : NULL;
         if (p && d)
            snprintf(prompt, sizeof(prompt), "%s\n\n%s", d, p);
         else if (p)
            snprintf(prompt, sizeof(prompt), "%s", p);
         else if (d)
            snprintf(prompt, sizeof(prompt), "%s", d);
         cJSON_Delete(ti);
      }
      if (strlen(prompt) >= 20)
      {
         cJSON *dreq = cJSON_CreateObject();
         cJSON_AddStringToObject(dreq, "role", "execute");
         cJSON_AddStringToObject(dreq, "persona", "engineer");
         cJSON_AddStringToObject(dreq, "prompt", prompt);
         if (sid && sid[0])
            cJSON_AddStringToObject(dreq, "session_id", sid);
         if (cwd && cwd[0])
            cJSON_AddStringToObject(dreq, "cwd", cwd);
         char derr[96] = "";
         int job_id = server_delegate_launch_async(ctx, NULL, dreq, derr, sizeof(derr));
         cJSON_Delete(dreq);
         if (job_id > 0)
            snprintf(msg, sizeof(msg),
                     "Sub-agents must run as aimee delegates, not provider-native Task/Agent. "
                     "Launched delegate job %d on your behalf — call delegate_status(job_id=%d) "
                     "for the result. For control over role/persona, call the delegate tool "
                     "directly next time.",
                     job_id, job_id);
      }
   }

   /* Skill review nudge: every N tool hooks, fire a background review delegate. */
   int skill_review_fire = 0;
   if (config_skills_review_enabled() && rc != 2 &&
       server_module_skill_should_fire(state.hook_call_count, config_skills_review_nudge_interval(),
                                       &skill_review_fire) == 0 &&
       skill_review_fire)
      server_compute_skill_review_async(ctx, sid);

   /* Build response */
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 2 ? "blocked" : "ok");
   cJSON_AddNumberToObject(resp, "exit_code", rc);
   cJSON_AddStringToObject(resp, "hook_identity", trusted_client ? "trusted" : "untrusted");
   if (msg[0])
      cJSON_AddStringToObject(resp, "message", msg);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);

   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   if (ti_heap)
      free(ti_heap);
   return src;
}

static int handle_hooks_post(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jtn = cJSON_GetObjectItemCaseSensitive(req, "tool_name");
   cJSON *jti = cJSON_GetObjectItemCaseSensitive(req, "tool_input");
   cJSON *jrid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
   const char *request_id = cJSON_IsString(jrid) ? jrid->valuestring : NULL;

   const char *sid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "session_id"));
   if (!sid || !is_safe_id(sid))
      return server_send_error(conn, "invalid session_id (must be alphanumeric/dash/underscore)",
                               request_id);
   const char *trusted_client = NULL;
   int hook_identity = server_hook_identity(conn, req, sid, &trusted_client);
   if (hook_identity < 0)
      return hook_send_blocked(conn, "Unauthenticated or mismatched session hook identity.",
                               request_id);

   const char *tool_name = cJSON_IsString(jtn) ? jtn->valuestring : "";
   char *ti_heap = NULL;
   const char *tool_input = "{}";

   if (cJSON_IsString(jti))
      tool_input = jti->valuestring;
   else if (cJSON_IsObject(jti) || cJSON_IsArray(jti))
   {
      ti_heap = cJSON_PrintUnformatted(jti);
      tool_input = ti_heap;
   }

   post_tool_update(tool_name, tool_input, NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "exit_code", 0);
   cJSON_AddStringToObject(resp, "hook_identity", trusted_client ? "trusted" : "untrusted");
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   if (ti_heap)
      free(ti_heap);
   return rc;
}

/* hooks.session_start: typed RPC for the SessionStart hook.
 * "hook_input" is the JSON written by the hook client to stdin.
 * "session_id" pins the aimee session for pre/post hooks.
 * "nonblocking": true offloads session_start_emit to a detached thread so
 * the ephemeral pool thread is not held while git operations complete. */
typedef struct
{
   char *hook_input;
   char sid[64];
} session_start_bg_t;

static void session_start_worktree_gc(const char *hook_input);

static void *session_start_bg_worker(void *arg)
{
   session_start_bg_t *a = (session_start_bg_t *)arg;
   if (a->sid[0])
      session_id_set_override(a->sid);
   FILE *dev_null = fopen("/dev/null", "w");
   session_start_emit(NULL, a->hook_input, dev_null ? dev_null : stderr);
   if (dev_null)
      fclose(dev_null);
   if (a->sid[0])
      session_id_clear_override();
   session_start_worktree_gc(a->hook_input);
   free(a->hook_input);
   free(a);
   return NULL;
}

static void session_start_worktree_gc(const char *hook_input)
{
   /* Auto-GC is gated on config (worktree_gc.enabled). The AIMEE_WORKTREE_GC
    * env var, when present, overrides the config flag in either direction. */
   int enabled = config_worktree_gc_enabled();
   const char *gc_env = getenv("AIMEE_WORKTREE_GC");
   if (gc_env && gc_env[0])
      enabled = (gc_env[0] == '1' || gc_env[0] == 't' || gc_env[0] == 'T');
   if (!enabled)
      return;

   cJSON *hi_json = hook_input && hook_input[0] ? cJSON_Parse(hook_input) : NULL;
   cJSON *jcwd = hi_json ? cJSON_GetObjectItemCaseSensitive(hi_json, "client_cwd") : NULL;
   const char *client_cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : NULL;
   if (client_cwd && client_cwd[0])
   {
      char git_root[MAX_PATH_LEN];
      if (git_repo_root(client_cwd, git_root, sizeof(git_root)) == 0)
      {
         time_t now = time(NULL);
         struct tm tm_utc;
         gmtime_r(&now, &tm_utc);
         char marker[MAX_PATH_LEN];
         snprintf(marker, sizeof(marker), "%s/.aimee/worktrees/.last-gc-%04d%02d%02d", git_root,
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
         struct stat st;
         if (stat(marker, &st) != 0)
         {
            worktree_gc_options_t opts;
            worktree_gc_options_init(&opts);
            if (config_worktree_gc_max_age_days() > 0)
               opts.max_age_days = config_worktree_gc_max_age_days();
            const char *days_env = getenv("AIMEE_WORKTREE_GC_DAYS");
            if (days_env && days_env[0])
            {
               int n = atoi(days_env);
               if (n > 0 && n <= 365)
                  opts.max_age_days = n;
            }
            worktree_gc_candidate_t cands[WORKTREE_GC_MAX_CANDIDATES];
            int n = worktree_gc_scan(git_root, &opts, cands, WORKTREE_GC_MAX_CANDIDATES);
            if (n > 0)
            {
               int removed = worktree_gc_apply(git_root, cands, n, &opts);
               if (removed > 0)
                  LOG_INFO("worktree_gc", "auto-GC removed %d worktree(s) under %s", removed,
                           git_root);
            }
            FILE *mf = fopen(marker, "w");
            if (mf)
               fclose(mf);
         }
      }
   }
   cJSON_Delete(hi_json);
}

static int handle_hooks_session_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jrid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
   const char *request_id = cJSON_IsString(jrid) ? jrid->valuestring : NULL;

   cJSON *jhi = cJSON_GetObjectItemCaseSensitive(req, "hook_input");
   const char *hook_input = (cJSON_IsString(jhi) && jhi->valuestring) ? jhi->valuestring : "";

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : NULL;
   if (sid && !is_safe_id(sid))
      return server_send_error(conn, "invalid session_id (must be alphanumeric/dash/underscore)",
                               request_id);

   const char *hook_client =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "harness_client"));
   if (hook_client && !hmem_scope_for_client(hook_client))
      return server_send_error(conn, "unknown harness_client", request_id);
   char hook_token[HOOK_SESSION_TOKEN_CAP] = "";
   time_t hook_token_expires = 0;
   if (sid && sid[0] && hook_client && hook_client[0])
   {
      char principal[128];
      server_hook_principal(conn, principal, sizeof(principal));
      if (hook_session_token_mint(sid, hook_client, principal, hook_token, &hook_token_expires) !=
          0)
         return server_send_error(conn, "could not establish hook identity", request_id);
   }

   /* Register the session in the server_sessions registry under its real host id
    * (session_start_emit persists session_state, but NOT this listings row). This
    * makes every session locatable after a crash — the gap that meant a Claude
    * Code session driven through the anonymous /v1/messages gateway left no
    * findable record. Idempotent (SessionStart also fires on resume); best-effort. */
   if (sid && sid[0])
   {
      db1_server_session_t existing;
      if (db1_server_session_get(sid, &existing) != 0)
      {
         char principal[32];
         snprintf(principal, sizeof(principal), "uid:%d", (int)conn->peer_uid);
         (void)db1_server_session_create(sid, hook_client && hook_client[0] ? hook_client : "hook",
                                         principal);
      }
   }

   cJSON *jnb = cJSON_GetObjectItemCaseSensitive(req, "nonblocking");
   if (cJSON_IsTrue(jnb))
   {
      session_start_bg_t *bg = malloc(sizeof(*bg));
      int launched = 0;
      if (bg)
      {
         bg->hook_input = strdup(hook_input);
         bg->sid[0] = '\0';
         if (sid && sid[0])
            snprintf(bg->sid, sizeof(bg->sid), "%s", sid);

         if (bg->hook_input)
         {
            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            launched = (pthread_create(&tid, &attr, session_start_bg_worker, bg) == 0);
            pthread_attr_destroy(&attr);
         }
         if (!launched)
         {
            free(bg->hook_input);
            free(bg);
         }
      }

      cJSON *resp = jo_ok();
      cJSON_AddNumberToObject(resp, "exit_code", 0);
      cJSON_AddStringToObject(resp, "output", "");
      if (hook_token[0])
      {
         cJSON_AddStringToObject(resp, "hook_token", hook_token);
         cJSON_AddNumberToObject(resp, "hook_token_expires_at", (double)hook_token_expires);
      }
      if (request_id)
         cJSON_AddStringToObject(resp, "request_id", request_id);
      return server_send_ok(conn, resp);
   }

   if (sid && sid[0])
      session_id_set_override(sid);

   char *captured = NULL;
   size_t captured_len = 0;
   FILE *mem = open_memstream(&captured, &captured_len);
   if (!mem)
   {
      session_id_clear_override();
      return server_send_error(conn, "open_memstream failed", request_id);
   }

   session_start_emit(NULL, hook_input, mem);
   fflush(mem);
   fclose(mem);
   session_id_clear_override();

   session_start_worktree_gc(hook_input);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "exit_code", 0);
   cJSON_AddStringToObject(resp, "output", captured ? captured : "");
   if (hook_token[0])
   {
      cJSON_AddStringToObject(resp, "hook_token", hook_token);
      cJSON_AddNumberToObject(resp, "hook_token_expires_at", (double)hook_token_expires);
   }
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);

   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   free(captured);
   return rc;
}

static int handle_hooks_session_end(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "request_id"));
   const char *sid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "session_id"));
   if (!sid || !is_safe_id(sid))
      return server_send_error(conn, "invalid session_id (must be alphanumeric/dash/underscore)",
                               request_id);
   const char *trusted_client = NULL;
   if (server_hook_identity(conn, req, sid, &trusted_client) != 1)
      return server_send_error(conn, "unauthenticated session hook identity", request_id);
   char principal[128];
   server_hook_principal(conn, principal, sizeof(principal));
   hook_session_token_revoke(sid, trusted_client, principal);
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "exit_code", 0);
   return server_send_ok(conn, resp);
}

/* session.brief_assemble: workspace-independent SessionStart brief for the
 * remote thin-client path (Proposal 1 Phase 1). Runs only session_brief_emit
 * (build_session_context) — no worktree/state/reindex side-effects, no
 * client_cwd filesystem access. Returns a minimal versioned envelope
 * {schema_version, output} so the thin client has a stable contract that Phase 2
 * can extend. Auth: session.* -> CAP_SESSION_READ. */
static int handle_session_brief_assemble(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jrid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
   const char *request_id = cJSON_IsString(jrid) ? jrid->valuestring : NULL;

   char *captured = NULL;
   size_t captured_len = 0;
   FILE *mem = open_memstream(&captured, &captured_len);
   if (!mem)
      return server_send_error(conn, "open_memstream failed", request_id);
   int active_context_missing = server_memory_scope_begin(req);
   session_brief_emit(mem);
   kb_client_memory_scope_context_clear();
   fflush(mem);
   fclose(mem);
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "schema_version", 1);
   cJSON_AddStringToObject(resp, "output", captured ? captured : "");
   cJSON_AddBoolToObject(resp, "active_context_missing", active_context_missing);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);

   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   free(captured);
   return rc;
}

/* memory.user_capture: upsert a per-user memory into db1 (Proposal 2 Phase 1
 * S2 — the write path behind `aimee memory identity/prefer`). db1 is per-user
 * by construction (aimee-server is 1:1 per user); this is how a thin client
 * populates the identity/preferences the session brief recalls. Params:
 * {kind, key, content, tier?}. CAP_MEMORY_WRITE. */
static int handle_memory_user_capture(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jrid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
   const char *request_id = cJSON_IsString(jrid) ? jrid->valuestring : NULL;
   const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "kind"));
   const char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "key"));
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "content"));
   const char *tier = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "tier"));
   const char *sid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "session_id"));
   if (!kind || !kind[0] || !key || !key[0])
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT, "kind and key are required",
                                    request_id);
   if (!content || !content[0])
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT, "content is required",
                                    request_id);
   if (db1_user_memory_upsert(kind, tier, key, content, 1.0, sid) != 0)
      return server_send_error_kind(conn, SERVER_ERR_UNAVAILABLE, "failed to store user memory",
                                    request_id);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "kind", kind);
   cJSON_AddStringToObject(resp, "key", key);
   cJSON_AddStringToObject(resp, "scope", "user");
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

static const server_method_dispatch_t server_dispatch_table[] = {
#include "server/server_dispatch_defs_data.h"
};

/* --- Dispatch --- */

/* Check JSON nesting depth (recursive) */
static int json_check_depth(const cJSON *item, int depth, int max_depth)
{
   if (depth > max_depth)
      return -1;
   if (!item)
      return 0;

   /* Check field count for objects */
   if (cJSON_IsObject(item))
   {
      int count = 0;
      const cJSON *child = item->child;
      while (child)
      {
         count++;
         if (count > JSON_MAX_FIELDS)
            return -2;
         if (json_check_depth(child, depth + 1, max_depth) != 0)
            return -1;
         child = child->next;
      }
   }
   else if (cJSON_IsArray(item))
   {
      const cJSON *child = item->child;
      while (child)
      {
         if (json_check_depth(child, depth + 1, max_depth) != 0)
            return -1;
         child = child->next;
      }
   }
   return 0;
}

/* Get per-method size limit based on method prefix */
static size_t method_size_limit(const char *method)
{
   if (!method)
      return LIMIT_DEFAULT;

   static const struct
   {
      const char *prefix;
      size_t max;
   } limits[] = {
       {"memory.", LIMIT_MEMORY},      {"tool.", LIMIT_TOOL},
       {"delegate", LIMIT_DELEGATE},   {"roundtable.review", LIMIT_ROUNDTABLE},
       {"mcp.call", LIMIT_DELEGATE},   {"chat.", LIMIT_CHAT},
       {"index.ingest", LIMIT_INGEST}, {"session.record_transcript", LIMIT_TRANSCRIPT},
       {NULL, LIMIT_DEFAULT},
   };

   for (int i = 0; limits[i].prefix; i++)
   {
      if (strncmp(method, limits[i].prefix, strlen(limits[i].prefix)) == 0)
         return limits[i].max;
   }
   return LIMIT_DEFAULT;
}

/* Above this an RPC earns an INFO access line of its own; below it DEBUG. */
#define SERVER_RPC_SLOW_MS 1000

int server_dispatch(server_ctx_t *ctx, server_conn_t *conn, const char *msg, size_t msg_len)
{
   /* Quick method extraction for size limit check (scan for "method":"..." in raw JSON) */
   const char *method_start = strstr(msg, "\"method\"");
   char quick_method[64] = "";
   if (method_start)
   {
      const char *val = strchr(method_start + 8, '"');
      if (val)
      {
         val++; /* skip opening quote */
         const char *end = strchr(val, '"');
         if (end && (size_t)(end - val) < sizeof(quick_method))
         {
            memcpy(quick_method, val, (size_t)(end - val));
            quick_method[end - val] = '\0';
         }
      }
   }

   /* Check per-method size limit before parsing */
   size_t limit = method_size_limit(quick_method);
   if (msg_len > limit)
   {
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg),
               "PAYLOAD_TOO_LARGE: %zu bytes exceeds %zu limit for method '%s'", msg_len, limit,
               quick_method);
      return server_send_error_kind(conn, SERVER_ERR_PAYLOAD_TOO_LARGE, errmsg, NULL);
   }

   /* cJSON accepts arbitrary bytes inside quoted strings. Reject them before
    * parsing so every JSON surface observes the same Unicode text contract. */
   if (strlen(msg) != msg_len || !text_is_valid_utf8(msg))
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT,
                                    "invalid JSON: input is not valid UTF-8", NULL);

   cJSON *req = cJSON_Parse(msg);
   if (!req)
      return server_send_error(conn, "invalid JSON", NULL);

   /* Check JSON depth and field count */
   if (json_check_depth(req, 0, JSON_MAX_DEPTH) != 0)
   {
      cJSON_Delete(req);
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT,
                                    "PAYLOAD_MALFORMED: JSON exceeds depth/field limits", NULL);
   }

   cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   if (!cJSON_IsString(method))
   {
      cJSON_Delete(req);
      return server_send_error(conn, "missing method", NULL);
   }

   const char *m = method->valuestring;
   int rc;

   /* Capability check */
   uint32_t required = server_capability_for_method(m);
   if (required && (conn->capabilities & required) == 0)
   {
      const method_policy_t *policy = server_policy_for_method(m);

      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "code", "AUTHZ_DENIED");
      cJSON_AddStringToObject(resp, "message", "forbidden: insufficient capabilities");
      cJSON_AddStringToObject(resp, "method", m);
      if (policy)
         cJSON_AddStringToObject(resp, "description", policy->description);

      /* Principal identifier */
      char principal[64];
      snprintf(principal, sizeof(principal), "uid:%u", (unsigned)conn->peer_uid);
      cJSON_AddStringToObject(resp, "principal", principal);

      cJSON_AddNumberToObject(resp, "required_caps", required);
      cJSON_AddNumberToObject(resp, "held_caps", conn->capabilities);

      /* Audit-relevant: authorization denial. */
      audit_log("AUTHZ_DENIED", "method=%s principal=%s required=0x%04x held=0x%04x", m, principal,
                required, conn->capabilities);

      int src = server_send_response(conn, resp);
      cJSON_Delete(resp);
      cJSON_Delete(req);
      return src;
   }

   rc = -1;
   const long long dispatch_started_ms = util_now_ms();
   for (int i = 0; server_dispatch_table[i].method; i++)
   {
      if (strcmp(m, server_dispatch_table[i].method) == 0)
      {
         rc = server_dispatch_table[i].handler(ctx, conn, req);
         break;
      }
   }
   /* Every HTTP request has an access line; this surface had none, so a call
    * that stalled here was invisible: the client reported only that it gave
    * up, and the log showed an idle server because it never recorded being
    * asked. Duration is the point -- 30s and 3ms are the same line without
    * it. Promote only slow or failed calls, as the HTTP line does for poll. */
   const long long dispatch_ms = util_now_ms() - dispatch_started_ms;
   if (rc < 0 || dispatch_ms >= SERVER_RPC_SLOW_MS)
      LOG_INFO("server.rpc", "%s -> rc=%d %lldms", m, rc, dispatch_ms);
   else
      LOG_DEBUG("server.rpc", "%s -> rc=%d %lldms", m, rc, dispatch_ms);
   if (rc == -1)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "code", "UNKNOWN_METHOD");
      cJSON_AddStringToObject(resp, "message", "unknown method");
      cJSON_AddStringToObject(resp, "method", m);
      rc = server_send_response(conn, resp);
      cJSON_Delete(resp);
   }

   cJSON_Delete(req);
   return rc;
}

/* The legacy NDJSON RPC accept loop, per-connection ephemeral workers, and
 * connection lifecycle (formerly server_conn.inc / server_ephemeral.inc) were
 * removed: aimee-server serves only the /v1 HTTP surface (server_http.c). The
 * shared response writers (conn_send/conn_flush/server_send_*) above are still
 * used by the in-process /v1 dispatch path via stack-allocated server_conn_t. */

/* --- Lifecycle --- */

/* Pid-file companion to the Unix socket. The connect-based probe in
 * server_init can wrongly report a busy server as "stale" — e.g. when
 * a long-running delegate request blocks the accept loop just long
 * enough that probe's non-blocking connect returns -1. The probe then
 * unlinks the socket and a second server binds to the same path,
 * leaving the original alive as an orphan listener. The pid file is
 * the deterministic check: if a recorded pid is still alive on this
 * box, the socket is NOT stale regardless of what probe says. */
static void server_pid_path(const char *socket_path, char *out, size_t out_len)
{
   /* Derive `<socket-dir>/aimee.pid` so the pid file lives next to the
    * socket and shares its filesystem permissions. */
   snprintf(out, out_len, "%s", socket_path);
   char *slash = strrchr(out, '/');
   if (slash)
      snprintf(slash + 1, out_len - (size_t)(slash - out) - 1, "aimee.pid");
   else
      snprintf(out, out_len, "aimee.pid");
}

static int server_pid_alive(const char *socket_path)
{
   char pid_path[1024];
   server_pid_path(socket_path, pid_path, sizeof(pid_path));
   FILE *f = fopen(pid_path, "r");
   if (!f)
      return 0;
   long pid_val = 0;
   int n = fscanf(f, "%ld", &pid_val);
   fclose(f);
   if (n != 1 || pid_val <= 1)
      return 0;
   /* The stale pid file may record OUR OWN pid: container process start order is
    * deterministic, so after a restart a fresh aimee-server is frequently
    * assigned the exact pid the previous instance wrote (the pid file lives on
    * the persisted AIMEE_HOME volume). Detecting ourselves as "another server"
    * would wedge every restart, so treat our own pid as not-a-conflict. */
   if (pid_val == (long)getpid())
      return 0;
   /* kill(pid, 0) returns 0 if the pid is alive (and we have permission
    * to signal), -1 with EPERM if alive but owned by another uid (still
    * counts as "alive" for our purposes), -1 with ESRCH if dead. */
   if (kill((pid_t)pid_val, 0) != 0 && errno != EPERM)
      return 0; /* ESRCH (or other): the pid is gone — stale pid file */
#ifdef __linux__
   /* The pid may be alive but a DIFFERENT process after a restart/reboot (PID
    * reuse) — common when the pid file lives on a persisted volume, e.g. the
    * Docker AIMEE_HOME volume: a fresh container reuses low PIDs, so the old
    * server's pid now belongs to some unrelated process and the bare kill(0)
    * above would wrongly report "already running" and block startup. Confirm the
    * pid is actually an aimee-server before treating it as a live instance; a
    * stale pid file then never wedges a restart. */
   char comm_path[64];
   snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid_val);
   FILE *cf = fopen(comm_path, "r");
   if (!cf)
      return 0; /* process vanished between checks — stale */
   char comm[64] = {0};
   char *got = fgets(comm, sizeof(comm), cf);
   fclose(cf);
   if (!got)
      return 0;
   comm[strcspn(comm, "\r\n")] = '\0';
   /* The kernel truncates comm to 15 chars; "aimee-server" (12) fits. */
   return strcmp(comm, "aimee-server") == 0;
#else
   return 1; /* alive; non-Linux keeps the kill-based liveness check */
#endif
}

/* Public liveness check: is an aimee-server instance running for `socket_path`?
 * Used by the offline `--rotate-master-key` path to refuse to mutate the vault
 * while the server is up (D13 F2). The caller resolves the default socket path. */
int server_is_running(const char *socket_path)
{
   return socket_path ? server_pid_alive(socket_path) : 0;
}

static void server_pid_write(const char *socket_path)
{
   char pid_path[1024];
   server_pid_path(socket_path, pid_path, sizeof(pid_path));
   FILE *f = fopen(pid_path, "w");
   if (!f)
      return;
   fprintf(f, "%ld\n", (long)getpid());
   fclose(f);
}

static void server_pid_clear(const char *socket_path)
{
   char pid_path[1024];
   server_pid_path(socket_path, pid_path, sizeof(pid_path));
   unlink(pid_path);
}

/* Route-time health predicate: exclude agents the provider catalog has marked
 * DOWN (3+ consecutive failures) from new delegate/agent routing. Registered
 * via agent_set_route_health_filter so agent_config.o needs no link dependency
 * on the catalog. Returns nonzero to exclude. */
static int server_agent_route_is_down(const char *agent_name)
{
   return provider_catalog_get_health(agent_name) == CATALOG_HEALTH_DOWN;
}

/* Route-time DEGRADED predicate (nonzero when degraded): a half-opened breaker
 * or intermittent failures. Not excluded - routing only PREFERS a healthy peer. */
static int server_agent_route_is_degraded(const char *agent_name)
{
   return provider_catalog_get_health(agent_name) == CATALOG_HEALTH_DEGRADED;
}

/* Route-time delegate-policy predicate (returns nonzero to EXCLUDE):
 * a per-agent "Primary Agent Only" choice (agents.json `primary_only`) excludes
 * the agent from ALL delegation. This is the SOLE per-agent gate: it replaced the
 * former global claude_cli_delegate_enabled opt-in AND the older unconditional
 * "the provider-named agent is never a delegation target" name match. That name
 * match made the operator's choice unreachable for the common claude-oauth-as-
 * primary setup — the OAuth flow always names the agent "claude", so an agent
 * named after config.provider could never be a delegate even with Primary Agent
 * Only unchecked. Now the flag alone decides: a claude-oauth subscription is
 * pre-flagged primary-only at add time (driving a personal Claude plan as an
 * automated delegate may breach Anthropic's terms), and unchecking it is an
 * explicit operator opt-in to self-delegation. Roundtable panels use this same
 * explicit agent policy plus review-role membership; they do not invent a
 * second hidden exclusion based on the configured primary provider.
 * The gate is config-independent (it reads the agent record), so it holds even
 * when legacy_config_read would fail. */
static int server_agent_route_policy_excluded(const agent_t *ag)
{
   if (!ag)
      return 1;
   /* A PRIMARY chat turn routes the provider-named agent through the same
    * machinery as delegation; the gate below is delegation policy, so it must
    * not exclude the primary from its own turn (it otherwise breaks every
    * server-side chat whose provider is a primary-only agent — the webchat's
    * default). The marker is thread-local to the chat worker. */
   if (agent_routing_primary_turn())
      return 0;
   if (ag->primary_only)
      return 1;
   return 0;
}

/* The shell-git gate (agent_tools.h): 1 = refuse this shell command, git belongs to
 * aimee. Lives here because the decision needs three things from three tiers that
 * the agent tool surface must not link — the config dial, the command classifier,
 * and the forge credential.
 *
 * Both extra conditions exist to keep the rule from becoming breakage:
 *  - a forge credential must EXIST, or aimee's own git cannot work either and this
 *    would take away the delegate's only route rather than redirect it;
 *  - the caller only reaches here when the git_* tools are registered (server.c
 *    wires this gate immediately after the provider), so we never forbid the shell
 *    while the alternative is absent.
 * An unreadable config reads as ON: a guard that fails open is not a guard. */
static int server_shell_git_blocked(const char *command, const char *cwd)
{
   if (!command || !command[0] || !wfe_shell_invokes_git("bash", command))
      return 0; /* not a git command — the overwhelmingly common case, stays silent */
   /* The gate exists to keep raw shell git off aimee-server's OWN filesystem and
    * push it through the credential-holding git_* tools. A delegate bound to its
    * own container runs its shell — including git — INSIDE that sandbox, against
    * the container's mounted worktree, not the host: the delegate is meant to
    * commit there and aimee collects the diff (push/PR still go through aimee at
    * the deliver stage). Blocking its local git is the "limitation on a container
    * delegate" this seam must not impose; allow it. */
   if (workspace_turn_container_bound())
   {
      aimee_log(LOG_DEBUG, "shell-git-gate",
                "allow: delegate is container-bound — git runs in its sandbox worktree, not on "
                "the host");
      return 0;
   }
   if (config_present() && !config_require_aimee_git())
   {
      aimee_log(LOG_DEBUG, "shell-git-gate", "allow: require_aimee_git is off (operator opt-out)");
      return 0;
   }
   /* "Has aimee-server got git configured?" is a property of the SERVER, not of the
    * directory this thread happens to sit in. Asking it per-repo made the SAME
    * command allowed from one cwd and refused from another — observed live on .254:
    * DENY inside a wfe worktree, then allow from the daemon's cwd twelve seconds
    * later, because the per-host rung only resolves where a checkout has an origin. */
   if (!git_cred_forge_configured())
   {
      /* The agent reached for git, the rule is ON, and we allowed it anyway because
       * aimee-server has no credential to offer instead. Deliberate — no aimee route,
       * no restriction — but never silent: a gate that has quietly stopped working
       * looks exactly like a gate nobody is testing, which is how two guards shipped
       * inert today. Server-level now, so it is the same answer every time. */
      aimee_log(LOG_INFO, "shell-git-gate",
                "allow: agent ran git in '%s' but aimee-server has NO forge credential "
                "configured — nothing to redirect to (no aimee route, no restriction)",
                (cwd && cwd[0]) ? cwd : "(no cwd)");
      return 0;
   }
   audit_log("shell-git-gate", "DENY cwd=%s cmd=%.120s", (cwd && cwd[0]) ? cwd : "-", command);
   return 1;
}

/* Boot-time posture, mirroring primary_cli_ingestor_log_posture: make "the rule is
 * on but nothing enforces it" visible at startup instead of leaving it to be
 * discovered on a box months later. Every condition is now a server-level fact, so
 * this states the real answer rather than a hopeful one. */
/* Say at boot whether the delegate sandbox can actually bite.
 *
 * Two guards shipped earlier today read correctly and were inert on the deployed
 * box, and neither said so — the whole reason this idiom exists. This one has a
 * worse failure than inertness: a sandbox believed-on but not binding means every
 * delegate runs its shell on the HOST, which is precisely what it was enabled to
 * prevent. An operator must not have to read the code to learn that. */
static void delegate_sandbox_log_posture(void)
{
   delegate_backend_t *b = delegate_backend_lookup("docker");
   if (!b || !b->acquire)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "UNAVAILABLE: the docker backend is not registered — no delegate can be given a "
                "container, so every delegation will REFUSE to run");
      return;
   }

   /* The backend being REGISTERED proves nothing: delegate_backend_register_docker()
    * only installs a vtable, so the lookup above succeeds on a box with no docker at
    * all. Reporting ARMED on that box would be the exact bug this whole idiom exists
    * to catch — a guard that reads correctly and cannot fire, saying nothing. So
    * probe the daemon the backend would actually use (honouring AIMEE_DOCKER_BIN,
    * which the tests' fake-docker fixture sets). */
   const char *bin = getenv("AIMEE_DOCKER_BIN");
   if (!bin || !bin[0])
      bin = "docker";
   const char *probe[] = {bin, "version", "--format", "{{.Server.Version}}", NULL};
   char *ver = NULL;
   const workspace_provider_t *sh = workspace_provider_shared();
   int rc = sh->exec(sh, probe, &ver, 256);
   if (rc != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "UNAVAILABLE: the docker backend is registered, but `%s version` failed (rc=%d) — "
                "no daemon reachable, so every delegation will REFUSE to run",
                bin, rc);
      free(ver);
      return;
   }
   if (ver)
   {
      char *nl = strchr(ver, '\n');
      if (nl)
         *nl = '\0';
   }
   aimee_log(LOG_INFO, "delegate-sandbox",
             "ARMED: delegate file/exec binds to a per-delegate container (docker server %s), "
             "created with --network none — no IP egress; its only outward channel is the "
             "bind-mounted aimee-server UDS (aimee_home/aimee-http.sock). The shell-git gate and "
             "credential strip remain complementary boundaries within that container.",
             (ver && ver[0]) ? ver : "?");
   free(ver);
}

static void server_shell_git_gate_log_posture(void)
{
   int dial_on = !config_present() || config_require_aimee_git();
   if (!dial_on)
   {
      aimee_log(LOG_INFO, "shell-git-gate",
                "OFF: require_aimee_git=false — delegates may run git/gh in a shell");
      return;
   }
   if (!agent_tools_git_write_provider())
   {
      aimee_log(LOG_WARN, "shell-git-gate",
                "INERT: require_aimee_git is on but the git_* tools are not registered — the "
                "rule would forbid the shell with nothing to redirect to, so it will not fire");
      return;
   }
   if (!git_cred_forge_configured())
   {
      aimee_log(LOG_WARN, "shell-git-gate",
                "INERT: require_aimee_git is on but aimee-server has NO forge credential "
                "configured — aimee's own git cannot work either, so the shell is not refused "
                "(no aimee route, no restriction)");
      return;
   }
   aimee_log(LOG_INFO, "shell-git-gate",
             "ARMED: git/gh in a delegate shell is refused; delegates use the git_* tools");
}

int server_init(server_ctx_t *ctx, const char *socket_path)
{
   memset(ctx, 0, sizeof(*ctx));
   ctx->listen_fd = -1;
   ctx->start_time = time(NULL);
   snprintf(ctx->socket_path, sizeof(ctx->socket_path), "%s", socket_path);
   /* Pid-file check beats connect-probe for liveness on a busy peer. */
   if (server_pid_alive(socket_path))
   {
      LOG_ERROR("server", "another server is already running (pid file)");
      errno = EEXIST;
      return -1;
   }

   /* The legacy NDJSON RPC listener was removed: aimee-server serves only the
    * /v1 HTTP surface (server_http.c binds its own UDS + optional TCP). No Unix
    * RPC socket is created here, so there is no stale-socket / symlink check and
    * no capability token to load. `fd` stays -1 so the shared error-cleanup paths
    * below remain valid no-ops (close(-1) and unlink of an absent path). */
   int fd = -1;
   /* Event loop retained for shutdown symmetry (platform_evloop_destroy); the
    * in-process /v1 dispatch path uses stack-allocated server_conn_t with no
    * evloop registration. */
   if (platform_evloop_create(&ctx->evloop) < 0)
   {
      int saved_errno = errno;
      perror("aimee-server: evloop create");
      errno = saved_errno;
      return -1;
   }
   /* Ensure the config dir (parent of the socket, pid file, and DB1 file)
    * exists before we write the pid file. On a fresh AIMEE_HOME (e.g. a deploy
    * not seeded by install.sh) nothing else has created it yet, so without this
    * server_pid_write silently fails -- and the module, which opens the DB1
    * file in that same directory, has nowhere to create it either. */
   {
      char cfg_dir[sizeof(ctx->socket_path)];
      snprintf(cfg_dir, sizeof(cfg_dir), "%s", socket_path);
      char *slash = strrchr(cfg_dir, '/');
      if (slash)
      {
         *slash = '\0';
         if (cfg_dir[0])
            platform_mkdir_p(cfg_dir, 0700);
      }
   }
   /* Record our pid so future server_init calls can detect us deterministically
    * (and `aimee server start/restart` can probe liveness). */
   server_pid_write(socket_path);
   /* This process opens no database. DB1 is a module, and every family it
    * serves is reached over the bus -- so the connection the server used to
    * hold here was one it never read or wrote through, and the cache and mmap
    * tuning that went with it was being applied to the one process where it
    * could not matter. The module applies it now.
    *
    * The three restart chores below reach the store over the bus. Each already
    * warns when it cannot, which is also what happens when the module has not
    * attached yet: nothing in this process launches it. */
   int orphaned = db1_agent_job_cancel_nonterminal_on_restart("orphaned by server restart");
   if (orphaned < 0)
      LOG_WARN("server", "failed to reconcile delegate jobs from the prior process");
   else if (orphaned > 0)
      LOG_INFO("server", "cancelled %d delegate jobs orphaned by the prior process", orphaned);
   if (server_mgmt_status_init() != 0)
      LOG_WARN("server", "management status nonce initialization failed");
   const char *trust_path = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   if (trust_path && trust_path[0] &&
       server_mgmt_jwks_cache_startup(trust_path, (int64_t)time(NULL),
                                      kb_client_mtls_management_jwks_fetch,
                                      NULL) != SERVER_MGMT_JWKS_CACHE_OK)
      LOG_WARN("server.mgmt", "management JWKS authorization unavailable");
   /* Container cleanup is independent of DB availability. No worker pool exists
    * yet, so a matching container cannot belong to this server generation. */
   int orphan_containers = delegate_backend_docker_remove_orphans();
   if (orphan_containers < 0)
      LOG_WARN("server", "could not reconcile delegate containers from the prior process");
   else if (orphan_containers > 0)
      LOG_INFO("server", "removed %d delegate containers orphaned by the prior process",
               orphan_containers);
   /* Seed personas + role templates so config (not code) is the source of truth. */
   server_seed_config_defaults();
   /* Credential environment variables are first-boot transport only. Seal them
    * before any capability posture checks or workers can consume them. */
   if (server_vault_bootstrap_prepare() < 0)
   {
      LOG_ERROR("server", "delegate credential Vault bootstrap failed");
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      return -1;
   }
   int compute_threads = aimee_resolve_compute_threads(config_compute_threads());
   int session_threads = aimee_resolve_session_threads(config_session_threads());
   /* Background (sessionless) delegates run on-demand, gated by the per-model
    * concurrency limiter; this only sets the pathological-fan-out backstop. */
   delegate_ondemand_set_ceiling(
       aimee_resolve_delegate_max_inflight(config_delegate_max_inflight()));
   /* Mutex for ctx->conns array (accept inserts; conn_close swap-shrinks). */
   pthread_mutex_init(&ctx->conns_mutex, NULL);
   /* Provider concurrency slots: global active count per agent. */
   /* Initialize the in-server compute thread pool. Long-running server-side
    * work (delegates, ingest) runs here; chat streams and tool callbacks use
    * bounded async lanes so callbacks can still get workers. */
   if (compute_pool_init(&ctx->pool, compute_threads) != 0)
   {
      LOG_ERROR("server", "failed to initialize compute pool");
      pthread_mutex_destroy(&ctx->conns_mutex);
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      unlink(socket_path);
      return -1;
   }
   if (pthread_mutex_init(&ctx->session_pools_mutex, NULL) != 0)
   {
      LOG_ERROR("server", "failed to initialize session pool mutex");
      compute_pool_shutdown(&ctx->pool);
      pthread_mutex_destroy(&ctx->conns_mutex);
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      unlink(socket_path);
      return -1;
   }
   ctx->session_threads = session_threads;
   ctx->session_pools_initialized = 1;
   int request_threads = server_request_pool_thread_count();
   if (compute_pool_init(&ctx->request_pool, request_threads) != 0)
   {
      LOG_ERROR("server", "failed to initialize request pool");
      server_session_pools_shutdown(ctx);
      compute_pool_shutdown(&ctx->pool);
      pthread_mutex_destroy(&ctx->conns_mutex);
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      unlink(socket_path);
      return -1;
   }
   ctx->request_pool_initialized = 1;
   compute_pool_register_secondary(&ctx->request_pool, "requests");
   /* Provider-backed orchestration is mostly blocked on remote I/O. Give it a
    * dedicated bounded lane so generic background work cannot consume its
    * coordinators; per-agent admission still owns actual provider concurrency. */
   if (compute_pool_init(&ctx->orchestration_pool, SERVER_ORCHESTRATION_POOL_THREADS) != 0)
   {
      LOG_ERROR("server", "failed to initialize orchestration pool");
      server_request_pool_shutdown(ctx);
      server_session_pools_shutdown(ctx);
      compute_pool_shutdown(&ctx->pool);
      pthread_mutex_destroy(&ctx->conns_mutex);
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      unlink(socket_path);
      return -1;
   }
   ctx->orchestration_pool_initialized = 1;
   compute_pool_register_secondary(&ctx->orchestration_pool, "orchestration");
   if (pthread_mutex_init(&ctx->compute_budget_mutex, NULL) != 0)
   {
      LOG_ERROR("server", "failed to initialize compute budget mutex");
      server_orchestration_pool_shutdown(ctx);
      server_request_pool_shutdown(ctx);
      server_session_pools_shutdown(ctx);
      compute_pool_shutdown(&ctx->pool);
      pthread_mutex_destroy(&ctx->conns_mutex);
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      unlink(socket_path);
      return -1;
   }
   if (pthread_cond_init(&ctx->compute_budget_cond, NULL) != 0)
   {
      LOG_ERROR("server", "failed to initialize compute budget condition");
      pthread_mutex_destroy(&ctx->compute_budget_mutex);
      server_orchestration_pool_shutdown(ctx);
      server_request_pool_shutdown(ctx);
      server_session_pools_shutdown(ctx);
      compute_pool_shutdown(&ctx->pool);
      pthread_mutex_destroy(&ctx->conns_mutex);
      close(fd);
      platform_evloop_destroy(&ctx->evloop);
      unlink(socket_path);
      return -1;
   }
   ctx->compute_budget_total = compute_threads;
   ctx->compute_budget_available = compute_threads;
   /* Populate the delegate-backend registry. local + ssh + docker
    * ship today. The dispatcher in delegate_driver does not yet
    * route through the registry — that's a later iteration. Failure
    * to register is non-fatal: the legacy local-exec path keeps
    * working, callers that explicitly opt into the new dispatcher
    * just won't find a backend. */
   if (delegate_backend_register_docker() != 0)
      LOG_WARN("server", "delegate_backend_register_docker failed (already registered?)");
   /* Log what's actually in the registry so operators can confirm
    * post-update.sh that new backends came through. The list is
    * also the read path the future `aimee delegate-backend list`
    * RPC will surface. */
   {
      delegate_backend_t *backends[DELEGATE_BACKEND_MAX] = {0};
      int n = delegate_backend_list(backends, DELEGATE_BACKEND_MAX);
      char names[256] = {0};
      size_t off = 0;
      for (int i = 0; i < n && off + 1 < sizeof(names); i++)
      {
         const char *name = backends[i] && backends[i]->name ? backends[i]->name : "?";
         off += (size_t)snprintf(names + off, sizeof(names) - off, "%s%s", off ? "," : "", name);
      }
      LOG_INFO("server", "delegate-backends registered: %d (%s)", n, n ? names : "none");
   }
   /* Keep new routed work off providers the health catalog has marked DOWN, so
    * a dead endpoint (e.g. an unreachable hosted model) doesn't wedge fresh
    * delegates. Routing falls back to a healthy peer; only when every candidate
    * for a role is DOWN does routing return a clean "no agent available". */
   agent_set_route_health_filter(server_agent_route_is_down);
   /* Prefer a healthy seat over a degraded one when both serve the role, so a
    * flapping seat stops winning on price alone while healthy peers exist. */
   agent_set_route_degraded_filter(server_agent_route_is_degraded);
   /* Delegate-policy invariants at every routing decision: the primary never
    * delegates to itself, and an agent flagged "Primary Agent Only"
    * (agents.json `primary_only`) is never a delegation target. */
   agent_set_route_policy_filter(server_agent_route_policy_excluded);
   /* Prefer a seat with a free slot over a saturated one (see agent_config.h). */
   agent_set_route_capacity_probe(agent_admission_agent_active);
   LOG_INFO("server",
            "initialized (v%s, protocol %d, background=%d session=%d threads); /v1 HTTP "
            "surface owns the listeners",
            AIMEE_VERSION, SERVER_PROTOCOL_VERSION, compute_threads, session_threads);
   trigger_scheduler_init();
   server_delegate_monitor_init();
   server_coord_dispatcher_init(ctx);
   /* WFE lifecycle is Go-only. This process exposes agent/roundtable resources
    * to the Go control plane but never registers a C workflow executor. */
   /* Give aimee's own agents the MCP tools marked native in mcp_tool_table. Must
    * precede any toolset_registry_init() / build_tools_array(), which snapshot the
    * registrations. aimee's agents and an external MCP client now reach the SAME
    * handler, so the two surfaces cannot drift apart: the drift is what left review
    * panelists unable to ask "is this still called?" while Claude Code could. */
   mcp_tool_register_native_surface();
   /* Hand the native agent surface aimee's git-write tools. Without this the
    * builtin set is read-only git, so a delegate's ONLY way to land work is to
    * shell out to `git` — the thing we then tell it not to do. The tools are
    * neither advertised nor dispatchable until this runs, which is deliberate:
    * only aimee-server can honour them (it owns the credential and the MCP git
    * rails), so a thin client or a unit test is never offered a tool that would
    * fail on it. */
   agent_tools_set_git_write_provider(mcp_git_run_tool);
   /* ...and only THEN forbid the shell route. Registered after the provider, and
    * only when refusing is honest: the tools we redirect to are now available, and
    * server_shell_git_blocked additionally requires a forge credential to exist.
    * A rule whose alternative is missing is breakage, so the alternative is wired
    * first and the rule hangs off it. */
   agent_tools_set_shell_git_gate(server_shell_git_blocked);
   /* Say at boot whether the rule can actually bite. Two guards shipped today read
    * correctly and were inert on the deployed box; neither said so. */
   server_shell_git_gate_log_posture();
   delegate_sandbox_log_posture();
   /* Boot-time enforcement-posture signal for the primary-CLI-ingestor: makes an
    * "enabled but silently inert" misconfig (flag on, dial off) visible at startup. */
   primary_cli_ingestor_log_posture();
   return 0;
}
int server_run(server_ctx_t *ctx)
{
   /* enforce-delegate-only: register the delegate-availability provider so the
    * gateway strips provider-native sub-agent tools whenever usable delegates
    * exist (CORE gateway_policy can't read agent state itself). */
   server_install_gateway_delegate_policy();

   /* Warm the webuser project-key registry (rebuild from published clones;
    * crash-window drift self-heals) and probe openat2 once, so the surface's
    * fail-closed gates are decided — and logged — before the first request.
    * ws_reg_ready() keeps the surface disabled on a failed rebuild and
    * retries lazily per request. */
   if (!ws_scope_openat2_available())
      aimee_log(LOG_WARN, "server",
                "openat2 unavailable on this kernel: the webuser project clone surface is "
                "disabled (fail closed)");
   else
      (void)ws_reg_ready();

   /* The /v1 HTTP listener (server_http.c) runs on its own accept thread with
    * per-connection workers, so the main thread has no NDJSON accept loop to
    * drive any more. Park here until a signal flips ctx->running, then return so
    * run_server() can tear everything down. The 1s tick also drives idle-reaping
    * of per-webuser code-server editors (WP-I lifecycle) via reap_tick. */
   while (ctx->running)
   {
      struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
      nanosleep(&ts, NULL);
      webuser_editor_reap_tick();
      /* SIGHUP requested a config reload (live-config-reload P1b): do it here, off the
       * signal path, since config_reload takes a mutex and does file I/O. */
      if (g_config_reload_requested)
      {
         g_config_reload_requested = 0;
         int rc = config_reload();
         aimee_log(rc < 0 ? LOG_WARN : LOG_INFO, "config", "SIGHUP config reload: %s",
                   rc > 0 ? "applied" : (rc == 0 ? "no change" : "rejected (kept running config)"));
         /* Also live-reload the TLS cert (re-read cert/key + swap SSL_CTX) so a renewed cert
          * is picked up on SIGHUP without dropping the listener (live-config-reload). */
         (void)server_tls_reload();
      }
      /* Live-config-reload P4: also pick up an OUT-OF-BAND file change (a CLI local
       * `config set`, a manual edit, or the autonomous config_save) — not just SIGHUP — so
       * the "operator toggle applies without a restart" contract holds however the file was
       * written. No-op tick when the file is unchanged. */
      int cfg_rc = config_reload_if_changed();
      if (cfg_rc != 0)
      {
         aimee_log(cfg_rc < 0 ? LOG_WARN : LOG_INFO, "config", "config file change: %s",
                   cfg_rc > 0 ? "reloaded" : "rejected (kept running config)");
      }
   }
   return 0;
}
void server_shutdown(server_ctx_t *ctx)
{
   /* Stop trigger scheduler before draining compute pool */
   trigger_scheduler_shutdown();
   server_delegate_monitor_shutdown();
   server_coord_dispatcher_shutdown();
   /* Reap any per-webuser code-server editors so they don't outlive us (WP-I). */
   webuser_editor_shutdown();
   /* Drain request handlers while compute/async lanes are still available for
    * any RPCs they dispatched. */
   server_request_pool_shutdown(ctx);
   /* Close orchestration admission first, but do not join coordinators until the
    * provider turns on which they may be waiting have been cancelled. */
   server_orchestration_pool_close(ctx);
   /* Cancel every in-flight turn BEFORE draining: turns now outlive their client
    * connections (server-owned turn lifecycle), so a long detached turn would
    * otherwise block the drain indefinitely. The atomic cancel flags are
    * observed by the workers within one poll tick; the drain then completes
    * bounded. Presence is torn down after the drain, so a still-running worker
    * never emits onto a closed ring. */
   turn_registry_begin_shutdown();
   /* Every queued/running async operation is now bounded by a pool worker and
    * every provider turn has observed cancellation. Drain before shared stores
    * and provider state are torn down. */
   server_orchestration_pool_shutdown(ctx);
   /* Let async chat/tool workers finish while the compute pool is still
    * available to drain any queued server-side jobs they interact with. */
   server_compute_async_drain();
   server_session_pools_shutdown(ctx);
   /* Give on-demand (sessionless) delegate threads a bounded window to finish.
    * They bypass the compute budget and own no socket, so any straggler past the
    * window only touches DB1 + its own ctx — safe to proceed. */
   delegate_ondemand_drain(5000);
   /* Shut down compute pool (drain in-flight work) */
   compute_pool_shutdown(&ctx->pool);
   pthread_cond_destroy(&ctx->compute_budget_cond);
   pthread_mutex_destroy(&ctx->compute_budget_mutex);
   pthread_mutex_destroy(&ctx->conns_mutex);
   /* No NDJSON listen socket to close any more; the /v1 HTTP listener is stopped
    * separately by server_http_stop() in run_server(). */
   platform_evloop_destroy(&ctx->evloop);
   /* Drop our pid file so a future server can detect that we are gone. */
   server_pid_clear(ctx->socket_path);
   /* Drain any audit rows still queued in the async writer, so rows enqueued
    * near shutdown are not lost (the writer thread is detached). The
    * request/compute pools are already drained above, so no new rows arrive.
    *
    * Nothing to close afterwards: this process opens no DB1 connection, and
    * the module closes its own when it stops. */
   agent_audit_async_flush();
   LOG_INFO("server", "shut down");
}
