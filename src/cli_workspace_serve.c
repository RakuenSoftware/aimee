/* cli_workspace_serve.c: `aimee workspace serve <id>` — the client-side runner
 * serve loop for a detached workspace (workspace-resource-plane §2-3).
 *
 * The filesystem-authority client long-polls the server for the next op it
 * needs done against the working tree (runner.poll), executes it locally via
 * the shared provider (ws_detached_runner_handle), and posts the result
 * (runner.respond). It is the client driver of the now-complete server-side
 * detached API; ws_runner_serve_once carries the per-op logic, with the poll /
 * respond transport injected here.
 *
 * Two transports: the local NDJSON Unix socket (co-located server), or — when a
 * remote /v1 endpoint is configured (AIMEE_API_ENDPOINT / aimee.api.client_*) —
 * the authenticated TCP twins POST /v1/runner/poll and POST /v1/runner/respond
 * (workspace-resource-plane §3), so a client can serve its working tree to a
 * server on another host (e.g. a container). */
#include "cli_client.h"
#include "platform_process.h"
#include "workspace_provider_detached.h"
#include "cJSON.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Threading for the background reverse-channel: pthreads co-located, the CRT's
 * _beginthreadex on Windows (always present in MinGW, no winpthreads needed). */
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifndef CLI_TUI_PATH_MAX
#define CLI_TUI_PATH_MAX 4096
#endif

typedef struct
{
   const char *sock;     /* local NDJSON socket path (NULL when remote) */
   const char *id;       /* workspace id */
   const char *endpoint; /* remote /v1 endpoint (NULL when local) */
   const char *bearer;   /* bearer for the remote endpoint */
} serve_ctx_t;

static volatile sig_atomic_t g_serve_stop = 0;
static void serve_on_signal(int sig)
{
   (void)sig;
   g_serve_stop = 1;
}

/* One short-lived dispatch to the co-located server over its first-class /v1
 * route (local aimee-http.sock). Returns the response (caller frees) or NULL. */
static cJSON *serve_rpc(const char *sock, cJSON *req, int timeout_ms)
{
   (void)sock; /* co-located runner reaches the server over the /v1 HTTP UDS */
   return cli_v1_dispatch_local(req, timeout_ms);
}

/* fetch: get the next op to execute (caller frees) or NULL when none is pending
 * / on error. Local: NDJSON runner.poll over the socket. Remote: POST
 * /v1/runner/poll {workspace_id} -> {ok, have_op, op?}. The server caps the wait
 * (~2s for /v1, ~25s for the socket); either way the serve loop re-polls. */
static cJSON *serve_fetch(void *vctx)
{
   serve_ctx_t *c = (serve_ctx_t *)vctx;

   if (c->endpoint)
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "workspace_id", c->id);
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      if (!body)
         return NULL;
      int status = 0;
      cJSON *resp =
          cli_http_request(c->endpoint, "POST", "/v1/runner/poll", body, c->bearer, 30000, &status);
      free(body);
      cJSON *op = NULL;
      if (resp && status >= 200 && status < 300 &&
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "have_op")))
      {
         cJSON *o = cJSON_GetObjectItemCaseSensitive(resp, "op");
         if (cJSON_IsObject(o))
            op = cJSON_Duplicate(o, 1);
      }
      cJSON_Delete(resp);
      return op;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "runner.poll");
   cJSON *args = cJSON_AddArrayToObject(req, "args");
   cJSON_AddItemToArray(args, cJSON_CreateString(c->id));

   cJSON *resp = serve_rpc(c->sock, req, 30000); /* > the server's ~25s long-poll */
   cJSON_Delete(req);

   cJSON *op = NULL;
   if (resp)
   {
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "have_op")))
      {
         cJSON *o = cJSON_GetObjectItemCaseSensitive(resp, "op");
         if (cJSON_IsObject(o))
            op = cJSON_Duplicate(o, 1);
      }
      cJSON_Delete(resp);
   }
   return op;
}

/* post: send the executed op's result back. Takes ownership of `response`.
 * Returns 0 on success, -1 otherwise. Local: NDJSON runner.respond. Remote:
 * POST /v1/runner/respond {workspace_id, response:{...}} -> {ok}. */
static int serve_post(void *vctx, cJSON *response)
{
   serve_ctx_t *c = (serve_ctx_t *)vctx;

   if (c->endpoint)
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "workspace_id", c->id);
      cJSON_AddItemToObject(req, "response", response); /* response now owned by req */
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req); /* frees the embedded response too */
      if (!body)
         return -1;
      int status = 0;
      cJSON *resp = cli_http_request(c->endpoint, "POST", "/v1/runner/respond", body, c->bearer,
                                     30000, &status);
      free(body);
      int ok = (resp != NULL && status >= 200 && status < 300);
      cJSON_Delete(resp);
      return ok ? 0 : -1;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "runner.respond");
   cJSON *args = cJSON_AddArrayToObject(req, "args");
   cJSON_AddItemToArray(args, cJSON_CreateString(c->id));
   cJSON_AddItemToObject(req, "response", response); /* response now owned by req */

   cJSON *resp = serve_rpc(c->sock, req, 30000);
   cJSON_Delete(req); /* frees the embedded response too */
   int ok = (resp != NULL);
   cJSON_Delete(resp);
   return ok ? 0 : -1;
}

/* The serve loop, factored out so callers other than `aimee workspace serve`
 * (e.g. mcp-serve's background reverse-channel thread) can drive it with their
 * own stop flag instead of the process-wide SIGINT/SIGTERM handler. Either
 * `sock` (local) or `endpoint` (remote) selects the transport. Returns 0 on a
 * clean stop, 1 if it bailed after too many consecutive transport errors. */
int cli_workspace_serve_loop(const char *workspace_id, const char *sock, const char *endpoint,
                             const char *bearer, volatile sig_atomic_t *stop)
{
   serve_ctx_t ctx = {sock, workspace_id, endpoint, bearer};
   int rc = 0;
   int consecutive_errors = 0;
   while (!*stop)
   {
      int r = ws_runner_serve_once(serve_fetch, serve_post, &ctx);
      if (r < 0)
      {
         if (++consecutive_errors >= 5)
         {
            rc = 1;
            break;
         }
      }
      else
      {
         consecutive_errors = 0;
      }
   }
   return rc;
}

int cmd_workspace_serve(const char *workspace_id)
{
   if (!workspace_id || !workspace_id[0])
   {
      fprintf(stderr, "usage: aimee workspace serve <workspace_id>\n");
      return 1;
   }

   /* Prefer a configured remote /v1 endpoint (serve the working tree to a server
    * on another host over the authenticated runner reverse-channel); otherwise
    * drive the co-located server over its local socket. */
   const char *sock = NULL;
   char *endpoint = NULL;
   char *bearer = NULL;
   if (cli_v1_has_remote_endpoint())
   {
      endpoint = cli_v1_client_endpoint();
      bearer = cli_v1_client_bearer();
   }
   if (!endpoint)
   {
      sock = cli_ensure_server_for_method("runner.poll");
      if (!sock)
      {
         fprintf(stderr, "aimee: server unavailable; cannot serve workspace\n");
         free(endpoint);
         free(bearer);
         return 1;
      }
   }

   platform_signal_int(serve_on_signal);
   platform_signal_term(serve_on_signal);

   fprintf(stderr, "aimee: serving detached workspace '%s' (%s) (Ctrl-C to stop)\n", workspace_id,
           endpoint ? endpoint : "local socket");

   int rc = cli_workspace_serve_loop(workspace_id, sock, endpoint, bearer, &g_serve_stop);
   if (rc)
      fprintf(stderr, "aimee: too many runner errors; stopping\n");

   if (!rc)
      fprintf(stderr, "\naimee: stopped serving '%s'\n", workspace_id);
   free(endpoint);
   free(bearer);
   return rc;
}

/* --- Reverse-channel helper for interactive/bridge commands ---
 *
 * When mcp-serve / chat target a remote aimee-server, the agent runs on the
 * server but its file/exec tools must act on THIS client's tree. These helpers
 * register the client's cwd as a `detached` workspace and serve it on a
 * background thread, so the server (which binds that workspace per turn by cwd)
 * marshals tool ops back here over runner.poll/respond. No-op for a co-located
 * server. One channel per process. */
static volatile sig_atomic_t g_rc_stop = 0;
#ifdef _WIN32
static HANDLE g_rc_thread = NULL;
#else
static pthread_t g_rc_thread;
#endif
static int g_rc_active = 0;
static int g_rc_unregister_on_stop = 0;
static char g_rc_workspace_id[CLI_TUI_PATH_MAX];
static char g_rc_endpoint[CLI_TUI_PATH_MAX + 32];
static char *g_rc_bearer = NULL;
struct rc_args
{
   char *id;
   char *endpoint;
   char *bearer;
};

static void rc_unregister_workspace_at(const char *endpoint, const char *bearer,
                                       const char *workspace_id)
{
   if (!workspace_id || !workspace_id[0] || !endpoint || !endpoint[0])
      return;

   char enc[CLI_TUI_PATH_MAX * 3];
   char path[sizeof(enc) + 32];
   if (cli_v1_pct_encode(workspace_id, enc, sizeof(enc)) == 0 &&
       snprintf(path, sizeof(path), "/v1/workspaces/%s", enc) < (int)sizeof(path))
   {
      int status = 0;
      cJSON *resp = cli_http_request(endpoint, "DELETE", path, NULL, bearer, 15000, &status);
      cJSON_Delete(resp);
   }
}

static void rc_unregister_workspace(void)
{
   if (g_rc_unregister_on_stop)
      rc_unregister_workspace_at(g_rc_endpoint, g_rc_bearer, g_rc_workspace_id);
   g_rc_unregister_on_stop = 0;
   g_rc_workspace_id[0] = '\0';
   g_rc_endpoint[0] = '\0';
   free(g_rc_bearer);
   g_rc_bearer = NULL;
}
/* Shared body for both thread ABIs: drive the serve loop, then free the args. */
static void rc_serve_run(struct rc_args *a)
{
   cli_workspace_serve_loop(a->id, NULL, a->endpoint, a->bearer, &g_rc_stop);
   free(a->id);
   free(a->endpoint);
   free(a->bearer);
   free(a);
}
#ifdef _WIN32
static unsigned __stdcall rc_thread_main(void *arg)
{
   rc_serve_run((struct rc_args *)arg);
   return 0;
}
#else
static void *rc_thread_main(void *arg)
{
   rc_serve_run((struct rc_args *)arg);
   return NULL;
}
#endif

int cli_workspace_reverse_channel_start(void)
{
   if (g_rc_active || !cli_v1_has_remote_endpoint())
      return 0;
   char cwd[CLI_TUI_PATH_MAX];
   if (!getcwd(cwd, sizeof(cwd)) || !cwd[0])
      return 0;
   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
      return 0;
   char *bearer = cli_v1_client_bearer();
   int should_unregister = 0;

   /* Register the detached workspace. Treat "already registered" as an
    * idempotent attach, but only tear down registrations created by this bridge. */
   cJSON *reg = cJSON_CreateObject();
   cJSON_AddStringToObject(reg, "root", cwd);
   cJSON_AddStringToObject(reg, "provider", "detached");
   char *body = cJSON_PrintUnformatted(reg);
   cJSON_Delete(reg);
   if (!body)
   {
      free(endpoint);
      free(bearer);
      return 0;
   }
   else
   {
      int status = 0;
      cJSON *resp =
          cli_http_request(endpoint, "POST", "/v1/workspaces", body, bearer, 15000, &status);
      free(body);
      if (status < 200 || status >= 300)
      {
         cJSON *msg = resp ? cJSON_GetObjectItemCaseSensitive(resp, "message") : NULL;
         int already = cJSON_IsString(msg) && strstr(msg->valuestring, "already registered");
         if (!already)
         {
            cJSON_Delete(resp);
            free(endpoint);
            free(bearer);
            return 0;
         }
      }
      else
      {
         should_unregister = 1;
      }
      cJSON_Delete(resp);
   }

   struct rc_args *a = calloc(1, sizeof(*a));
   if (!a)
   {
      if (should_unregister)
         rc_unregister_workspace_at(endpoint, bearer, cwd);
      free(endpoint);
      free(bearer);
      return 0;
   }
   a->id = strdup(cwd);
   if (!a->id)
   {
      if (should_unregister)
         rc_unregister_workspace_at(endpoint, bearer, cwd);
      free(a);
      free(endpoint);
      free(bearer);
      return 0;
   }
   a->endpoint = endpoint; /* ownership passes to the thread */
   a->bearer = bearer;
   snprintf(g_rc_workspace_id, sizeof(g_rc_workspace_id), "%s", cwd);
   snprintf(g_rc_endpoint, sizeof(g_rc_endpoint), "%s", endpoint);
   g_rc_unregister_on_stop = should_unregister;
   free(g_rc_bearer);
   g_rc_bearer = bearer ? strdup(bearer) : NULL;
   if (bearer && !g_rc_bearer)
   {
      if (should_unregister)
         rc_unregister_workspace_at(endpoint, bearer, cwd);
      free(a->id);
      free(a);
      free(endpoint);
      free(bearer);
      g_rc_workspace_id[0] = '\0';
      g_rc_endpoint[0] = '\0';
      g_rc_unregister_on_stop = 0;
      return 0;
   }
   g_rc_stop = 0;
#ifdef _WIN32
   g_rc_thread = (HANDLE)_beginthreadex(NULL, 0, rc_thread_main, a, 0, NULL);
   if (g_rc_thread)
   {
      g_rc_active = 1;
      return 1;
   }
#else
   if (pthread_create(&g_rc_thread, NULL, rc_thread_main, a) == 0)
   {
      g_rc_active = 1;
      return 1;
   }
#endif
   free(a->id);
   free(a->endpoint);
   free(a->bearer);
   free(a);
   rc_unregister_workspace();
   return 0;
}

void cli_workspace_reverse_channel_stop(void)
{
   if (!g_rc_active)
      return;
   g_rc_stop = 1;
   /* The poll may be mid-flight (~30s); the process is exiting, so detach
    * rather than block on join. */
#ifdef _WIN32
   if (g_rc_thread)
   {
      CloseHandle(g_rc_thread);
      g_rc_thread = NULL;
   }
#else
   pthread_detach(g_rc_thread);
#endif
   g_rc_active = 0;
   rc_unregister_workspace();
}
