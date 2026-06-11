/* server.c: aimee-server core -- event loop, connection handling, method dispatch */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "server.h"
#include "server_http.h" /* server_http_api_status_report */
#include "config.h"      /* config_t / config_load for api.status, api.enable */
#include "delegate_backend_docker.h"
#include "delegate_backend_local.h"
#include "delegate_backend_ssh.h"
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
#include "skill_review.h"
#include "trigger_scheduler.h"
#include "server_trigger.h"
#include "server_cron.h"
#include "commands.h"
#include "agent.h"
#include "agent_config.h"
#include "provider_catalog.h"
#include "delegate_credentials.h"
#include "model_registry.h"
#include "model_provider.h"
#include "model_registry.h"
#include "db1.h"
#include "token_audit.h"
#include "dashboard.h"
#include "log.h"
#include "hud.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "util.h"
#include "workspace.h"
#include "worktree_gc.h"
#include "git_verify.h"
#include "toolset.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include "server_seed_config.inc" /* server_seed_config_defaults() */

extern int hooks_ensure_cwd_worktree(session_state_t *state, const char *sid, const char *cwd);

typedef int (*server_method_handler_t)(server_ctx_t *, server_conn_t *, cJSON *);
typedef struct
{
   const char *method;
   server_method_handler_t handler;
} server_method_dispatch_t;
#define SERVER_REQUEST_POOL_MAX_THREADS 4
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

/* Update event loop registration: IN always, OUT when there's pending data */
static void conn_update_events(server_conn_t *conn)
{
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
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   return server_send_ok(conn, resp);
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
   cJSON_AddStringToObject(resp, "state", db1_is_initialized() ? "ok" : "unavailable");
   cJSON_AddNumberToObject(resp, "connections", ctx->conn_count);
   return server_send_ok(conn, resp);
}

#include "server_insights.inc"
#include "server_api_status.inc"

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

/* Drive one backend through acquire/exec/release. Operator-facing —
 * useful for smoke-testing the registry end-to-end without touching
 * the legacy delegate flow. Request fields:
 *   backend       (required) — registry key, e.g. "local"
 *   task_id       (required) — workspace identifier
 *   command       (required) — bash command line for exec()
 *   image         (optional) — docker image hint
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

   char *quoted = shell_escape(kb_path);
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "'%s' --bootstrap-db2 --json", quoted);
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
   char sid[16];
   {
      unsigned char rnd[4];
      if (platform_random_bytes(rnd, sizeof(rnd)) != 0)
      {
         rnd[0] = (unsigned char)(getpid() & 0xff);
         rnd[1] = (unsigned char)((getpid() >> 8) & 0xff);
         rnd[2] = (unsigned char)(time(NULL) & 0xff);
         rnd[3] = (unsigned char)((time(NULL) >> 8) & 0xff);
      }
      snprintf(sid, sizeof(sid), "%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3]);
   }

   config_t cfg;
   config_load(&cfg);

   const char *provider = cfg.provider[0] ? cfg.provider : "claude";
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
   if (strcmp(provider, "claude") == 0 && cfg.claude_model[0])
      cJSON_AddStringToObject(launch_resp, "model", cfg.claude_model);
   else if ((strcmp(provider, "codex") == 0 || strcmp(provider, "codex-oauth") == 0 ||
             strcmp(provider, "chatgpt") == 0) &&
            cfg.codex_model[0])
      cJSON_AddStringToObject(launch_resp, "model", cfg.codex_model);
   cJSON_AddBoolToObject(launch_resp, "builtin", builtin);
   if (cfg.autonomous)
      cJSON_AddBoolToObject(launch_resp, "autonomous", 1);
   if (target_dir[0])
      cJSON_AddStringToObject(launch_resp, "worktree_cwd", target_dir);

   return server_send_ok(conn, launch_resp);
}

#include "server_provider.inc"
#include "server_provider_slots.inc"
#include "server_eval.inc"

static int handle_init_run(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *cwd = NULL;
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
      cwd = jcwd->valuestring;

   cJSON *resp = jo_ok();
   cJSON_AddBoolToObject(resp, "local_ready", db1_is_initialized() ? 1 : 0);
   cJSON_AddBoolToObject(resp, "db1_ready", db1_is_initialized() ? 1 : 0);

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
   if (!db1_is_initialized())
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

static int handle_hooks_pre(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

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

   /* Load config */
   config_t cfg;
   config_load(&cfg);

   /* Load session state from DB1 using provided session_id */
   session_state_t state;
   session_state_load(&state, sid);
   hooks_ensure_cwd_worktree(&state, sid, cwd);

   /* Run guardrail check */
   char msg[1024] = "";
   int rc = pre_tool_check(tool_name, tool_input, &state, config_guardrail_mode(&cfg), cwd, msg,
                           sizeof(msg));

   session_state_save(&state, sid);

   /* Skill review nudge: every N tool hooks, fire a background review delegate. */
   if (cfg.skills_review_enabled && rc != 2 &&
       skill_review_should_fire(state.hook_call_count, cfg.skills_review_nudge_interval))
      server_compute_skill_review_async(ctx, sid);

   /* Build response */
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 2 ? "blocked" : "ok");
   cJSON_AddNumberToObject(resp, "exit_code", rc);
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
   const char *gc_env = getenv("AIMEE_WORKTREE_GC");
   if (!gc_env || (gc_env[0] != '1' && gc_env[0] != 't' && gc_env[0] != 'T'))
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
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);

   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   free(captured);
   return rc;
}

static const server_method_dispatch_t server_dispatch_table[] = {
    /* Server */
    {"server.info", handle_server_info},
    {"server.health", handle_server_health},
    {"api.status", handle_api_status},
    {"api.enable", handle_api_enable},
    {"api.disable", handle_api_disable},
    {"insights.overview", handle_insights_overview},
    {"toolset.list", handle_toolset_list},
    {"toolset.show", handle_toolset_show},
    {"toolset.resolve", handle_toolset_resolve},
    {"worktree.gc", handle_worktree_gc},
    {"delegate.backend_list", handle_delegate_backend_list},
    {"delegate.backend_exec", handle_delegate_backend_exec},
    {"provider.slot_acquire", handle_provider_slot_acquire},
    {"provider.slot_release", handle_provider_slot_release},
    {"provider.list", handle_provider_list},
    {"provider.show", handle_provider_show},
    {"provider.models", handle_provider_models},
    {"provider.test", handle_provider_test},
    {"provider.quota", handle_provider_quota},
    {"provider.get", handle_provider_get},
    {"provider.set", handle_provider_set},
    {"model.list", handle_model_list},
    {"model.show", handle_model_show},
    {"model.refresh", handle_model_refresh},
    {"init.run", handle_init_run},
    {"launch.run", handle_launch_run},
    {"hud.status", handle_hud_status},
    {"agent.list", handle_agent_list},
    {"agent.add", handle_agent_add},
    {"agent.local", handle_agent_local},
    {"agent.remove", handle_agent_remove},
    {"agent.enable", handle_agent_enable},
    {"agent.disable", handle_agent_disable},
    {"agent.probe", handle_agent_probe},
    {"agent.setup", handle_agent_setup},
    {"agent.setup_poll", handle_agent_setup_poll},
    {"hooks.pre", handle_hooks_pre},
    {"hooks.post", handle_hooks_post},
    {"hooks.session_start", handle_hooks_session_start},
    {"session.create", handle_session_create},
    {"session.list", handle_session_list},
    {"session.credentials", handle_session_credentials},
    {"session.get", handle_session_get},
    {"session.close", handle_session_close},
    {"session.brief", handle_session_brief},
    {"session.attach", handle_session_attach},
    {"session.detach", handle_session_detach},
    {"session.presence", handle_session_presence},
    {"trajectory.export", handle_trajectory_export},
    {"trajectory.batch", handle_trajectory_batch},
    /* Memory */
    {"memory.search", handle_memory_search},
    {"memory.store", handle_memory_store},
    {"memory.list", handle_memory_list},
    {"memory.stats", handle_memory_stats},
    {"memory.get", handle_memory_get},
    {"memory.read", handle_memory_read},
    {"memory.benchmark", handle_memory_benchmark},
    {"index.scan", handle_index_scan},
    {"index.ingest", handle_index_ingest},
    {"index.find", handle_index_find},
    {"index.list", handle_index_list},
    {"index.blast_radius", handle_index_blast_radius},
    {"index.structure", handle_index_structure},
    {"index.find_callers", handle_index_find_callers},
    {"graph.sync_code", handle_graph_sync_code},
    {"graph.explain", handle_graph_explain},
    {"blast_radius.preview", handle_blast_radius_preview},
    /* KB */
    {"kb.search", handle_kb_search},
    {"curator.implements", handle_curator_implements},
    {"curator.synthesize", handle_curator_synthesize},
    {"curator.contradictions", handle_curator_contradictions},
    {"curator.invalidated", handle_curator_invalidated},
    {"kb.build", handle_kb_build},
    {"kb.update", handle_kb_update},
    {"kb.docs.push", handle_kb_docs_push},
    {"kb.ingest", handle_kb_ingest},
    {"kb.ingest.status", handle_kb_ingest_status},
    {"kb.status", handle_kb_status},
    {"optimize.export", handle_optimize_export},
    {"optimize.promote", handle_optimize_promote},
    {"optimize.replay_record", handle_optimize_replay_record},
    {"calibration.readiness", handle_calibration_readiness},
    {"demotion.check", handle_demotion_check},
    {"workers", handle_workers},
    /* Rules */
    {"rules.list", handle_rules_list},
    {"rules.generate", handle_rules_generate},
    {"rules.delete", handle_rules_delete},
    /* Skills */
    {"skill.list", handle_skill_list},
    {"skill.show", handle_skill_show},
    {"skill.lint", handle_skill_lint},
    {"skill.eval", handle_skill_eval},
    {"skill.create", handle_skill_create},
    {"skill.edit", handle_skill_edit},
    {"skill.patch", handle_skill_patch},
    {"skill.archive", handle_skill_archive},
    {"skill.pin", handle_skill_pin},
    {"skill.unpin", handle_skill_pin},
    {"skill.lifecycle", handle_skill_lifecycle},
    {"skill.autostub", handle_skill_autostub},
    {"collab_rules.list", handle_collab_rules_list},
    {"collab_rules.list_active", handle_collab_rules_list_active},
    {"collab_rules.approve", handle_collab_rules_approve},
    {"collab_rules.reject", handle_collab_rules_reject},
    {"collab_rules.retire", handle_collab_rules_retire},
    /* Working memory */
    {"wm.set", handle_wm_set},
    {"wm.get", handle_wm_get},
    {"wm.list", handle_wm_list},
    {"wm.context", handle_wm_context},
    /* Per-session primary agent selection */
    {"primary.set", handle_primary_set},
    {"primary.get", handle_primary_get},
    {"primary.clear", handle_primary_clear},
    /* Work queue */
    {"work.add", handle_work_add},
    {"work.add_batch", handle_work_add_batch},
    {"work.claim", handle_work_claim},
    {"work.complete", handle_work_complete},
    {"work.fail", handle_work_fail},
    {"work.list", handle_work_list},
    {"work.board", handle_work_board},
    {"work.cancel", handle_work_cancel},
    {"work.release", handle_work_release},
    {"work.clear", handle_work_clear},
    {"work.gc", handle_work_gc},
    {"work.sync_proposals", handle_work_sync_proposals},
    {"work.stats", handle_work_stats},
    /* Attempt log */
    {"attempt.record", handle_attempt_record},
    {"attempt.list", handle_attempt_list},
    /* Dashboard */
    {"dashboard.metrics", handle_dashboard_metrics},
    {"dashboard.delegations", handle_dashboard_delegations},
    {"dashboard.traces", handle_dashboard_traces},
    {"dashboard.plans", handle_dashboard_plans},
    {"dashboard.logs", handle_dashboard_logs},
    {"dashboard.plugins", handle_dashboard_plugins},
    {"plugin.list", handle_plugin_list},
    {"plugin.enable", handle_plugin_enable},
    {"plugin.disable", handle_plugin_disable},
    {"dashboard.onboard", handle_dashboard_onboard},
    {"dashboard.memory_stats", handle_dashboard_memory_stats},
    {"dashboard.all", handle_dashboard_all},
    {"lsp.diagnostics_summary", handle_lsp_diagnostics_summary},
    {"workspace.context", handle_workspace_context},
    {"workspace.add", handle_workspace_add},
    {"workspace.list", handle_workspace_list},
    {"workspace.get", handle_workspace_get},
    {"workspace.remove", handle_workspace_remove},
    {"workspace.mirror-sync", handle_workspace_mirror_sync},
    {"runner.poll", handle_runner_poll},
    {"runner.respond", handle_runner_respond},
    {"dogfood.tag", handle_dogfood_tag},
    {"dogfood.review", handle_dogfood_review},
    {"dogfood.report", handle_dogfood_report},
    {"eval.run", handle_eval_run},
    {"eval.results", handle_eval_results},
    /* Identity */
    {"identity.show", handle_identity_show},
    {"identity.snapshot", handle_identity_snapshot},
    {"identity.diff", handle_identity_diff},
    /* Compute (thread pool) */
    {"tool.execute", handle_tool_execute},
    {"delegate", handle_delegate},
    /* Public /v1 delegate aggregate/roundtable routes enqueue through
     * rh_dispatch_op_async. Direct raw dispatch remains synchronous for
     * compatibility with the dispatch-method surface. */
    {"delegate.aggregate", handle_delegate_aggregate},
    {"delegate.roundtable", handle_delegate_roundtable},
    {"delegate.launch", handle_delegate_launch},
    {"delegate.status", handle_delegate_status},
    {"jobs.list", handle_jobs_list},
    {"jobs.status", handle_jobs_status},
    {"jobs.logs", handle_jobs_logs},
    {"jobs.cancel", handle_jobs_cancel},
    {"job.start", handle_coord_job_start},
    {"job.list", handle_coord_job_list},
    {"job.status", handle_coord_job_status},
    {"job.cancel", handle_coord_job_cancel},
    {"aux.config_show", handle_aux_config_show},
    {"config.show", handle_config_show},
    {"config.get", handle_config_get},
    {"config.set", handle_config_set},
    {"aux.test", handle_aux_test},
    {"delegate.reply", handle_delegate_reply},
    {"delegate.log", handle_delegate_log},
    {"episode.list", handle_episode_list},
    {"agent.episodes", handle_agent_episodes},
    {"chat.send_stream", handle_chat_send_stream},
    /* MCP proxy */
    {"mcp.tools_list", handle_mcp_tools_list},
    {"mcp.audit", handle_mcp_audit},
    {"mcp.recheck", handle_mcp_recheck},
    {"mcp.call", handle_mcp_call},
    /* Triggers */
    {"trigger.fire", handle_trigger_fire},
    {"trigger.list", handle_trigger_list},
    {"trigger.status", handle_trigger_status},
    {"trigger.cancel", handle_trigger_cancel},
    {"cron.list", handle_cron_list},
    {"cron.add", handle_cron_add},
    {"cron.show", handle_cron_show},
    {"cron.history", handle_cron_history},
    {"cron.run", handle_cron_run},
    {"cron.enable", handle_cron_enable},
    {"cron.disable", handle_cron_disable},
    {"cron.remove", handle_cron_remove},
    {NULL, NULL},
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
       {"memory.", LIMIT_MEMORY},    {"tool.", LIMIT_TOOL}, {"delegate", LIMIT_DELEGATE},
       {"mcp.call", LIMIT_DELEGATE}, {"chat.", LIMIT_CHAT}, {"index.ingest", LIMIT_INGEST},
       {NULL, LIMIT_DEFAULT},
   };

   for (int i = 0; limits[i].prefix; i++)
   {
      if (strncmp(method, limits[i].prefix, strlen(limits[i].prefix)) == 0)
         return limits[i].max;
   }
   return LIMIT_DEFAULT;
}

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
      return server_send_error(conn, errmsg, NULL);
   }

   cJSON *req = cJSON_Parse(msg);
   if (!req)
      return server_send_error(conn, "invalid JSON", NULL);

   /* Check JSON depth and field count */
   if (json_check_depth(req, 0, JSON_MAX_DEPTH) != 0)
   {
      cJSON_Delete(req);
      return server_send_error(conn, "PAYLOAD_MALFORMED: JSON exceeds depth/field limits", NULL);
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
   for (int i = 0; server_dispatch_table[i].method; i++)
   {
      if (strcmp(m, server_dispatch_table[i].method) == 0)
      {
         rc = server_dispatch_table[i].handler(ctx, conn, req);
         break;
      }
   }
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
    * exists before we write the pid file or open DB1. On a fresh AIMEE_HOME
    * (e.g. a deploy not seeded by install.sh) nothing else has created it yet,
    * so without this both server_pid_write and db1_init silently fail and DB1
    * stays unavailable for the whole process lifetime. */
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
   /* Initialize DB1 (aimee-server is DB1's exclusive owner). */
   config_t cfg;
   config_load(&cfg);
   if (db1_init(cfg.db1_path) != 0)
      LOG_WARN("server", "db1_init failed for %s — DB1-backed handlers will be unavailable",
               cfg.db1_path);
   else
      db1_apply_server_pragmas();
   /* Seed personas + role templates so config (not code) is the source of truth. */
   server_seed_config_defaults();
   int compute_threads = aimee_resolve_compute_threads(cfg.compute_threads);
   int session_threads = aimee_resolve_session_threads(cfg.session_threads);
   /* Mutex for ctx->conns array (accept inserts; conn_close swap-shrinks). */
   pthread_mutex_init(&ctx->conns_mutex, NULL);
   /* Provider concurrency slots: global active count per agent. */
   pthread_mutex_init(&ctx->provider_slots_mutex, NULL);
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
   if (pthread_mutex_init(&ctx->compute_budget_mutex, NULL) != 0)
   {
      LOG_ERROR("server", "failed to initialize compute budget mutex");
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
   if (delegate_backend_register_local() != 0)
      LOG_WARN("server", "delegate_backend_register_local failed (already registered?)");
   if (delegate_backend_register_ssh() != 0)
      LOG_WARN("server", "delegate_backend_register_ssh failed (already registered?)");
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
   LOG_INFO("server",
            "initialized (v%s, protocol %d, background=%d session=%d threads); /v1 HTTP "
            "surface owns the listeners",
            AIMEE_VERSION, SERVER_PROTOCOL_VERSION, compute_threads, session_threads);
   trigger_scheduler_init();
   server_delegate_monitor_init();
   server_coord_dispatcher_init(ctx);
   return 0;
}
int server_run(server_ctx_t *ctx)
{
   /* The /v1 HTTP listener (server_http.c) runs on its own accept thread with
    * per-connection workers, so the main thread has no NDJSON accept loop to
    * drive any more. Park here until a signal flips ctx->running, then return so
    * run_server() can tear everything down. */
   while (ctx->running)
   {
      struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
      nanosleep(&ts, NULL);
   }
   return 0;
}
void server_shutdown(server_ctx_t *ctx)
{
   /* Stop trigger scheduler before draining compute pool */
   trigger_scheduler_shutdown();
   server_delegate_monitor_shutdown();
   server_coord_dispatcher_shutdown();
   /* Drain request handlers while compute/async lanes are still available for
    * any RPCs they dispatched. */
   server_request_pool_shutdown(ctx);
   /* Let async chat/tool workers finish while the compute pool is still
    * available to drain any queued server-side jobs they interact with. */
   server_compute_async_drain();
   server_session_pools_shutdown(ctx);
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
   db1_shutdown();
   LOG_INFO("server", "shut down");
}
