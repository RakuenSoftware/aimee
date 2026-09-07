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
#include "cli_v1_routes_internal.h"
#include "platform_process.h"
#include "platform_random.h"
#include "workspace_provider_detached.h"
#include "util.h" /* safe_exec_capture: the client's own vcs coordinates */
#include <aimee/workspace/client_diff.h>
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
   int warned_unserved;  /* the "no runner registered" notice is said once */
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
      /* `served:false` means the server has no runner registered for this tree,
       * so nothing will ever be handed to us. Say so once rather than polling
       * in silence: this loop has no backoff of its own by design (it relies on
       * the server capping the wait), and the operator's real problem is that
       * this serve loop is pointed at a tree the server does not know about.
       * Older servers omit the field, so absence is treated as served. */
      const cJSON *served = resp ? cJSON_GetObjectItemCaseSensitive(resp, "served") : NULL;
      if (cJSON_IsBool(served) && !cJSON_IsTrue(served) && !c->warned_unserved)
      {
         c->warned_unserved = 1;
         fprintf(stderr,
                 "aimee workspace serve: the server has no runner registered for \"%s\"; "
                 "it will hand this loop no work. Re-register the workspace, or stop serving it.\n",
                 c->id);
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
static int cli_workspace_serve_loop_impl(const char *workspace_id, const char *sock,
                                         const char *endpoint, const char *bearer,
                                         volatile sig_atomic_t *stop, int quiet_unserved)
{
   serve_ctx_t ctx = {sock, workspace_id, endpoint, bearer, quiet_unserved ? 1 : 0};
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

int cli_workspace_serve_loop(const char *workspace_id, const char *sock, const char *endpoint,
                             const char *bearer, volatile sig_atomic_t *stop)
{
   return cli_workspace_serve_loop_impl(workspace_id, sock, endpoint, bearer, stop, 0);
}

/* A one-shot remote CLI command cannot block on its own HTTP response AND poll
 * the detached runner queue on the same thread.  Historically only the
 * long-running `workspace serve` command (and MCP bridge) drove that queue, so
 * `aimee git status` against a correctly registered detached workspace waited
 * forever unless the operator happened to start a second process by hand.
 *
 * The scoped helper below finds the longest registered detached root containing
 * the current directory and serves it on a background thread for exactly one
 * forwarded Git command.  It never registers, converts, or removes a workspace;
 * the server remains the authority for the provider choice. */
static volatile sig_atomic_t g_git_runner_stop;
static int g_git_runner_active;
static char g_git_runner_workspace[CLI_TUI_PATH_MAX];
static char g_git_runner_endpoint[CLI_TUI_PATH_MAX + 32];
static char *g_git_runner_bearer;
#ifdef _WIN32
static HANDLE g_git_runner_thread;
static unsigned __stdcall git_runner_thread_main(void *unused)
#else
static pthread_t g_git_runner_thread;
static void *git_runner_thread_main(void *unused)
#endif
{
   (void)unused;
   (void)cli_workspace_serve_loop_impl(g_git_runner_workspace, NULL, g_git_runner_endpoint,
                                       g_git_runner_bearer, &g_git_runner_stop,
                                       1 /* a pre-request empty poll is expected */);
#ifdef _WIN32
   return 0;
#else
   return NULL;
#endif
}

static int workspace_root_contains(const char *root, const char *cwd)
{
   if (!root || !cwd || root[0] != '/' || cwd[0] != '/')
      return 0;
   size_t n = strlen(root);
   if (n == 1)
      return 1;
   while (n > 1 && root[n - 1] == '/')
      n--;
   return strncmp(root, cwd, n) == 0 && (cwd[n] == '\0' || cwd[n] == '/');
}

int cli_workspace_git_runner_start(void)
{
   if (g_git_runner_active || !cli_v1_has_remote_endpoint())
      return 0;

   char cwd[CLI_TUI_PATH_MAX];
   if (!getcwd(cwd, sizeof(cwd)) || cwd[0] != '/')
      return -1;
   char *endpoint = cli_v1_client_endpoint();
   char *bearer = endpoint ? cli_v1_client_bearer() : NULL;
   if (!endpoint)
   {
      free(bearer);
      return 0;
   }

   int status = 0;
   cJSON *list = cli_http_request(endpoint, "GET", "/v1/workspaces", NULL, bearer, 15000, &status);
   if (!list || status < 200 || status >= 300)
   {
      cJSON_Delete(list);
      free(endpoint);
      free(bearer);
      return -1;
   }

   const char *best = NULL;
   size_t best_len = 0;
   const cJSON *arr = cJSON_GetObjectItemCaseSensitive(list, "workspaces");
   const cJSON *row = NULL;
   cJSON_ArrayForEach(row, arr)
   {
      const cJSON *provider = cJSON_GetObjectItemCaseSensitive(row, "provider");
      const cJSON *path = cJSON_GetObjectItemCaseSensitive(row, "path");
      if (!cJSON_IsString(provider) || strcmp(provider->valuestring, "detached") != 0 ||
          !cJSON_IsString(path) || !workspace_root_contains(path->valuestring, cwd))
         continue;
      size_t n = strlen(path->valuestring);
      if (n > best_len)
      {
         best = path->valuestring;
         best_len = n;
      }
   }
   if (!best)
   {
      cJSON_Delete(list);
      free(endpoint);
      free(bearer);
      return 0; /* shared/mirror/unregistered: no detached runner to service */
   }
   if (best_len >= sizeof(g_git_runner_workspace) ||
       strlen(endpoint) >= sizeof(g_git_runner_endpoint))
   {
      cJSON_Delete(list);
      free(endpoint);
      free(bearer);
      return -1;
   }
   snprintf(g_git_runner_workspace, sizeof(g_git_runner_workspace), "%s", best);
   snprintf(g_git_runner_endpoint, sizeof(g_git_runner_endpoint), "%s", endpoint);
   cJSON_Delete(list);
   free(endpoint);
   g_git_runner_bearer = bearer;
   g_git_runner_stop = 0;

#ifdef _WIN32
   uintptr_t handle = _beginthreadex(NULL, 0, git_runner_thread_main, NULL, 0, NULL);
   if (!handle)
   {
      free(g_git_runner_bearer);
      g_git_runner_bearer = NULL;
      return -1;
   }
   g_git_runner_thread = (HANDLE)handle;
#else
   if (pthread_create(&g_git_runner_thread, NULL, git_runner_thread_main, NULL) != 0)
   {
      free(g_git_runner_bearer);
      g_git_runner_bearer = NULL;
      return -1;
   }
#endif
   g_git_runner_active = 1;
   return 1;
}

void cli_workspace_git_runner_stop(void)
{
   if (!g_git_runner_active)
      return;
   g_git_runner_stop = 1;
#ifdef _WIN32
   WaitForSingleObject(g_git_runner_thread, INFINITE);
   CloseHandle(g_git_runner_thread);
   g_git_runner_thread = NULL;
#else
   (void)pthread_join(g_git_runner_thread, NULL);
#endif
   g_git_runner_active = 0;
   g_git_runner_workspace[0] = '\0';
   g_git_runner_endpoint[0] = '\0';
   free(g_git_runner_bearer);
   g_git_runner_bearer = NULL;
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
 * register the client's cwd as a `mirror` workspace and ship its diff, so the
 * server reconstructs the tree on ITS OWN filesystem and delegates work there.
 * No-op for a co-located server. One channel per process.
 *
 * NO SERVE LOOP. This used to register `detached` and drive one on a background
 * thread, and the loop outlived that decision: a mirror is reconstructed
 * server-side and the runner rendezvous a serve loop waits on is created ONLY
 * for a detached provider (workspace_turn.c), so for a mirror it is never
 * created at all. The thread polled /v1/runner/poll forever against a tree
 * nobody serves — every answer "no runner", re-polled at once — which is what
 * produced ~196k of 200k server log lines and a failed module stage per poll.
 * `aimee workspace serve` still drives a real loop; that is an operator
 * deliberately serving a tree, and it registers `detached`. */
static int g_rc_active = 0;
static int g_rc_unregister_on_stop = 0;
static char g_rc_workspace_id[CLI_TUI_PATH_MAX];
static char g_rc_endpoint[CLI_TUI_PATH_MAX + 32];
static char *g_rc_bearer = NULL;

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
/* The VCS coordinates the mirror tier seeds from: the fetch URL of `origin` and
 * the current HEAD commit. BOTH are required — the server reconstructs the tree
 * by fetching THIS head from THIS remote — so a directory that is not a repo,
 * has no `origin`, or has no commit yet cannot be mirrored. Returns 1 only when
 * both resolved. POSIX-only: the thin Windows client cannot fork git, so the
 * helper below lives inside the guard too — outside it, it is an unused static
 * and this tree builds with -Werror. */
#if !defined(_WIN32) && !defined(_WIN64)
/* Strip trailing whitespace/newline from a captured git one-liner in place. */
static void rc_chomp(char *s)
{
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
      s[--n] = '\0';
}

static int rc_mirror_coords(const char *root, char *remote, size_t rcap, char *head, size_t hcap,
                            char *branch, size_t bcap, char *upstream, size_t ucap)
{
   remote[0] = '\0';
   head[0] = '\0';
   branch[0] = '\0';
   upstream[0] = '\0';
   char *out = NULL;
   const char *ru[] = {"git", "-C", root, "remote", "get-url", "origin", NULL};
   if (safe_exec_capture(ru, &out, 1024) == 0 && out)
   {
      snprintf(remote, rcap, "%s", out);
      rc_chomp(remote);
   }
   free(out);
   /* The head we register is the mirror BASE — the newest ancestor of HEAD that
    * a remote already has — not HEAD itself. The server reconstructs by fetching
    * this commit, so registering an unpushed HEAD would name something it can
    * never resolve. Local commits are not lost: they ride along in the patch
    * shipped below, which is computed against this same base. */
   if (workspace_client_mirror_base(root, head, hcap) != 0)
      head[0] = '\0';
   out = NULL;
   const char *br[] = {"git", "-C", root, "symbolic-ref", "--quiet", "--short", "HEAD", NULL};
   if (safe_exec_capture(br, &out, 512) == 0 && out)
   {
      snprintf(branch, bcap, "%s", out);
      rc_chomp(branch);
   }
   free(out);
   out = NULL;
   const char *up[] = {
       "git", "-C", root, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}", NULL};
   if (safe_exec_capture(up, &out, 512) == 0 && out)
   {
      snprintf(upstream, ucap, "%s", out);
      rc_chomp(upstream);
   }
   free(out);
   return remote[0] && head[0];
}
#else
static int rc_mirror_coords(const char *root, char *remote, size_t rcap, char *head, size_t hcap,
                            char *branch, size_t bcap, char *upstream, size_t ucap)
{
   (void)root;
   (void)rcap;
   (void)hcap;
   (void)bcap;
   (void)ucap;
   remote[0] = '\0';
   head[0] = '\0';
   branch[0] = '\0';
   upstream[0] = '\0';
   return 0;
}
#endif

/* Ship the client's working-tree patch for `root` so the server's reconstruct
 * matches what the client actually has: everything between `base` and the
 * working tree, which is unpushed commits AND uncommitted edits AND untracked
 * files. Without it the reconstruct is a clean checkout at base and all of that
 * is silently missing from the tree the agent works in — the failure this whole
 * path exists to avoid.
 *
 * `base` is the commit just registered as the workspace head. Passed in rather
 * than re-resolved so the patch cannot be computed against a different commit
 * than the one the server will check out; a patch and its base are one fact.
 *
 * Best-effort: a failure is reported, never fatal, because a tree at base is
 * still a usable (if stale) sandbox and the drift report will say so. */
/* A transfer id the server will accept: exactly 32 lowercase hex characters.
   It only has to be unique among concurrent transfers for one workspace, so
   random bytes are enough and a collision would be caught by the server's own
   sequence check rather than silently merging two patches. */
static int rc_transfer_id(char *out, size_t cap)
{
   unsigned char raw[16];
   if (cap < 33 || platform_random_bytes(raw, sizeof raw) != 0)
      return -1;
   for (size_t i = 0; i < sizeof raw; i++)
      snprintf(out + i * 2u, cap - i * 2u, "%02x", raw[i]);
   out[32] = '\0';
   return 0;
}

/* One chunk of the patch, as its own mirror-sync request. seq 0 begins the
 * transfer, later ones append, and `final` publishes the assembled diff. Every
 * chunk repeats head/branch/upstream because the server reads them from
 * whichever request it is handling and the final one validates head. */
static int rc_ship_chunk(const char *endpoint, const char *bearer, const char *root,
                         const char *base, const char *branch, const char *upstream,
                         const char *transfer, int seq, int final, const char *slice,
                         size_t slice_len)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "method", "workspace.mirror-sync");
   cJSON *args = cJSON_CreateArray();
   cJSON_AddItemToArray(args, cJSON_CreateString(root));
   cJSON_AddItemToObject(req, "args", args);
   cJSON_AddStringToObject(req, "head", base ? base : "");
   cJSON_AddStringToObject(req, "branch", branch ? branch : "");
   cJSON_AddStringToObject(req, "upstream", upstream ? upstream : "");
   char *slice_z = malloc(slice_len + 1);
   if (!slice_z)
   {
      cJSON_Delete(req);
      return -1;
   }
   memcpy(slice_z, slice, slice_len);
   slice_z[slice_len] = '\0';
   cJSON_AddStringToObject(req, "diff", slice_z);
   free(slice_z);
   if (transfer)
   {
      cJSON_AddStringToObject(req, "transfer", transfer);
      cJSON_AddNumberToObject(req, "seq", seq);
      cJSON_AddBoolToObject(req, "final", final);
   }
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   /* The listener drops a body over its ceiling without reading it, and the
      client then sees only EPIPE on a write it had not finished -- reported as
      "could not reach the endpoint", which blames a server that is up and
      answering. Refuse it here, where the size is known and can be said. */
   size_t body_len = strlen(body);
   if (body_len >= CLI_V1_MAX_BODY)
   {
      fprintf(stderr,
              "aimee: a single mirror-sync chunk came to %zu bytes, over the %d-byte request "
              "ceiling; the working-tree diff cannot be shipped\n",
              body_len, (int)CLI_V1_MAX_BODY);
      free(body);
      return -1;
   }

   const char *verb = "POST";
   const char *path = cli_v1_route_for_method("workspace.mirror-sync", &verb);
   if (!path)
   {
      free(body);
      fprintf(stderr, "aimee: no route for workspace.mirror-sync; the server-side sandbox will "
                      "NOT contain uncommitted work\n");
      return -1;
   }
   int status = 0;
   cJSON *resp = cli_http_request(endpoint, verb, path, body, bearer, 60000, &status);
   free(body);
   int ok = status >= 200 && status < 300;
   if (!ok)
      fprintf(stderr,
              "aimee: could not ship the working-tree diff for %s (HTTP %d, chunk %d); the "
              "server-side sandbox will be a clean checkout at HEAD and will NOT contain "
              "uncommitted work\n",
              root, status, seq);
   cJSON_Delete(resp);
   return ok ? 0 : -1;
}

/* Ship the client's working-tree patch for `root` so the server's reconstruct
 * matches what the developer actually has: everything between `base` and the
 * working tree, which is unpushed commits AND uncommitted edits AND untracked
 * files.
 *
 * That is unbounded. A tree a few commits ahead can produce megabytes, and the
 * whole patch used to go in one request: over the listener's ceiling it was
 * dropped unread, the client hit EPIPE partway through writing and reported
 * "HTTP 0", and the sandbox silently became a clean checkout at HEAD. The
 * server has always been able to reassemble a chunked transfer -- seq, final
 * and a transfer id -- and this is the client that uses it.
 *
 * `base` is the commit just registered as the workspace head. Passed in rather
 * than re-resolved so the patch cannot be computed against a different commit
 * than the one the server will check out; a patch and its base are one fact.
 *
 * Best-effort: a failure is reported, never fatal, because a tree at base is
 * still a usable (if stale) sandbox and the drift report will say so. */
static int rc_ship_client_diff(const char *endpoint, const char *bearer, const char *root,
                               const char *base, const char *branch, const char *upstream)
{
   char *patch = workspace_client_diff_compute(root, base);
   const char *text = patch ? patch : "";
   size_t total = strlen(text);

   /* Raw bytes per chunk. Well under the ceiling so that JSON escaping, which
      can lengthen a byte into six, cannot push a chunk over it. */
   const size_t chunk_max = 512u * 1024u;

   int rc;
   if (total <= chunk_max)
   {
      /* One request, and deliberately WITHOUT seq/final: a server that predates
         chunking treats a bare request as a whole transfer, which is what this
         has always sent. */
      rc = rc_ship_chunk(endpoint, bearer, root, base, branch, upstream, NULL, 0, 1, text, total);
   }
   else
   {
      char transfer[33];
      if (rc_transfer_id(transfer, sizeof transfer) != 0)
      {
         free(patch);
         fprintf(stderr, "aimee: could not name a mirror-sync transfer; the working-tree diff "
                         "cannot be shipped\n");
         return -1;
      }
      rc = 0;
      int seq = 0;
      for (size_t at = 0; at < total && rc == 0; at += chunk_max, seq++)
      {
         size_t len = total - at < chunk_max ? total - at : chunk_max;
         int final = (at + len >= total);
         rc = rc_ship_chunk(endpoint, bearer, root, base, branch, upstream, transfer, seq, final,
                            text + at, len);
      }
   }
   free(patch);
   return rc;
}

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

   /* Register as `mirror`: the server seeds a bare mirror from this repo's
    * remote, reconstructs a worktree at this head, applies the client diff
    * shipped below, and delegates work THERE.
    *
    * That placement is the point. The thin client is always remote from
    * aimee-server (mTLS), but a delegate runs LOCAL to the server — so the tree
    * it needs must exist on the server's own filesystem, which is exactly what
    * the mirror reconstructs. `detached` produces the opposite: the tree stays
    * on this machine and every file and shell op marshals back to it, so an
    * agent edits the developer's live working copy. The delegate path refuses
    * that rather than reach across — a detached workspace is never given a
    * container (server_compute.c) — which is why such a delegate landed in an
    * empty scratch dir and could not see the repo at all.
    *
    * So there is no `detached` fallback here. A root the mirror cannot seed is
    * refused: the reverse channel does not start and the operator is told what
    * is missing. Failing closed leaves a delegate with no workspace, which is
    * visible; failing open leaves it editing the developer's files, which is
    * not.
    *
    * (`aimee workspace serve` still registers `detached` explicitly. That is an
    * operator deliberately serving a tree, not an automatic default.) */
   char ws_remote[512] = "", ws_head[128] = "", ws_branch[256] = "", ws_upstream[256] = "";
   if (!rc_mirror_coords(cwd, ws_remote, sizeof(ws_remote), ws_head, sizeof(ws_head), ws_branch,
                         sizeof(ws_branch), ws_upstream, sizeof(ws_upstream)))
   {
      fprintf(stderr,
              "aimee: %s cannot be mirrored (needs a git repository with an `origin` remote and at "
              "least one commit), so no workspace was registered. Agents get no sandbox rather "
              "than access to this working tree. Add an origin remote and commit, then reattach.\n",
              cwd);
      free(endpoint);
      free(bearer);
      return 0;
   }

   /* Register the workspace. Treat "already registered" as an idempotent attach,
    * but only tear down registrations created by this bridge. */
   cJSON *reg = cJSON_CreateObject();
   cJSON_AddStringToObject(reg, "root", cwd);
   cJSON_AddStringToObject(reg, "provider", "mirror");
   cJSON_AddStringToObject(reg, "remote", ws_remote);
   cJSON_AddStringToObject(reg, "head", ws_head);
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

   /* Ship the working-tree patch before any turn can bind the workspace: the
    * first reconstruct applies whatever diff is on the server at that moment,
    * and an absent one is a silent clean checkout at head. Runs on the attach
    * path too ("already registered"), because a re-attaching client's tree has
    * usually moved on since the registration that created it. */
   rc_ship_client_diff(endpoint, bearer, cwd, ws_head, ws_branch, ws_upstream);

   /* Record what a later stop() needs to tear the registration down. The channel
    * is "active" from here: the registration and the shipped diff ARE the
    * channel for a mirror workspace. There is no thread to start. */
   snprintf(g_rc_workspace_id, sizeof(g_rc_workspace_id), "%s", cwd);
   snprintf(g_rc_endpoint, sizeof(g_rc_endpoint), "%s", endpoint);
   g_rc_unregister_on_stop = should_unregister;
   free(g_rc_bearer);
   g_rc_bearer = bearer ? strdup(bearer) : NULL;
   if (bearer && !g_rc_bearer)
   {
      if (should_unregister)
         rc_unregister_workspace_at(endpoint, bearer, cwd);
      free(endpoint);
      free(bearer);
      g_rc_workspace_id[0] = '\0';
      g_rc_endpoint[0] = '\0';
      g_rc_unregister_on_stop = 0;
      return 0;
   }
   g_rc_active = 1;
   free(endpoint);
   free(bearer);
   return 1;
}

int cli_workspace_reverse_channel_sync(void)
{
   /* Co-located MCP and calls made outside a registered Git workspace need no
    * mirror refresh.  Once a remote mirror channel is active, however, a failed
    * refresh must stop the Git call rather than silently use its older snapshot. */
   if (!g_rc_active)
      return 0;
   char head[128] = "", remote[512] = "", branch[256] = "", upstream[256] = "";
   if (!rc_mirror_coords(g_rc_workspace_id, remote, sizeof(remote), head, sizeof(head), branch,
                         sizeof(branch), upstream, sizeof(upstream)))
      return -1;
   return rc_ship_client_diff(g_rc_endpoint, g_rc_bearer, g_rc_workspace_id, head, branch,
                              upstream);
}

void cli_workspace_reverse_channel_stop(void)
{
   if (!g_rc_active)
      return;
   /* Nothing to join: the mirror channel is a registration plus a shipped diff,
    * not a running thread. Tearing down the registration is the whole stop. */
   g_rc_active = 0;
   rc_unregister_workspace();
}
