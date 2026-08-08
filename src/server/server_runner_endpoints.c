/* server_runner_endpoints.c: split from server_state.c into a real translation unit
 * (was server_runner_endpoints.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "server_state_internal.h"
#include "workspace_scan_indexed.h"
#include "aimee.h"
#include "server.h"
#include "dashboard.h"
#include "lsp.h"
#include "platform_path.h"
#include <aimee/workspace/workspace.h>
#include "modules/workspace/workspace_mirror.h"
#include "modules/workspace/workspace_provider.h"
#include "modules/workspace/workspace_handle.h"
#include "modules/workspace/workspace_runner_registry.h"
#include "modules/git/forge_credentials.h"
#include "db1.h"
#include "kb_client.h"
#include "compute_pool.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "dogfood.h"
#include "commands.h"
#include "platform_path.h"
#include "server_http.h"  /* session_primary_set/get/clear */
#include "agent_config.h" /* agent_load_config / agent_find */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* workspace.get: read-only manifest for a workspace handle — which provider
 * backs it, its root, existence, and VCS state (workspace-resource-plane §1). */
int handle_workspace_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char *argv[8];
   int argc = workspace_rpc_args(req, argv, 8);
   if (argc < 1 || !argv[0][0])
      return server_send_error(conn, "usage: workspace get <path>", NULL);

   char root[MAX_PATH_LEN];
   if (!realpath(argv[0], root))
      snprintf(root, sizeof(root), "%s", argv[0]); /* report the path even if absent */

   const workspace_provider_t *ws = workspace_provider_shared();
   ws_stat_t st;
   ws->stat(ws, root, &st);

   /* Report the registered provider for this root (default `shared`), and the
    * registry-recorded VCS coordinates for a `mirror` workspace whose root does
    * not exist server-side (so the live git probe below would find nothing). */
   const char *provider = "shared";
   char reg_remote[512] = "", reg_head[128] = "";
   {
      int ws_n = config_workspace_count();
      for (int i = 0; i < ws_n; i++)
         if (strcmp(config_workspaces(i), root) == 0)
         {
            if (config_workspace_providers(i)[0])
               provider = ws_provider_kind_to_string(
                   ws_provider_kind_from_string(config_workspace_providers(i)));
            snprintf(reg_remote, sizeof(reg_remote), "%s", config_workspace_vcs_remote(i));
            snprintf(reg_head, sizeof(reg_head), "%s", config_workspace_vcs_head(i));
            break;
         }
   }

   char remote[512] = "", head[128] = "", branch[128] = "";
   if (st.exists && st.is_dir)
   {
      const char *a_remote[] = {"git", "-C", root, "config", "--get", "remote.origin.url", NULL};
      const char *a_head[] = {"git", "-C", root, "rev-parse", "HEAD", NULL};
      const char *a_branch[] = {"git", "-C", root, "rev-parse", "--abbrev-ref", "HEAD", NULL};
      ws_git_line(a_remote, remote, sizeof(remote));
      ws_git_line(a_head, head, sizeof(head));
      ws_git_line(a_branch, branch, sizeof(branch));
   }
   /* Fall back to the registered coordinates when the root is not a live repo. */
   if (!remote[0] && reg_remote[0])
      snprintf(remote, sizeof(remote), "%s", reg_remote);
   if (!head[0] && reg_head[0])
      snprintf(head, sizeof(head), "%s", reg_head);

   cJSON *manifest =
       workspace_manifest_json(root, provider, st.exists, st.is_dir, remote, head, branch);
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "manifest", manifest);
   return send_and_free(conn, resp);
}

/* workspace.add: register a workspace in the server-side registry (the thin
 * client has no local config). Split out of server_state.c for its line budget;
 * shares workspace_rpc_args + jo_ok/send_and_free defined there. A `mirror`
 * workspace records the client vcs.remote + head the lifecycle seeds from. */
int handle_workspace_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char *argv[8];
   int argc = workspace_rpc_args(req, argv, 8);
   if (argc < 1 || !argv[0][0])
      return server_send_error(conn, "usage: workspace add <path>", NULL);

   /* Optional `--provider <shared|detached|mirror>` (defaults to shared). A
    * `mirror` workspace also takes `--remote <url>` (required) and `--head <sha>`
    * — the client VCS coordinates the server seeds its bare mirror from. */
   const char *provider = NULL;
   const char *remote = NULL;
   const char *head = NULL;
   for (int i = 1; i + 1 < argc; i++)
   {
      if (strcmp(argv[i], "--provider") == 0)
         provider = argv[i + 1];
      else if (strcmp(argv[i], "--remote") == 0)
         remote = argv[i + 1];
      else if (strcmp(argv[i], "--head") == 0)
         head = argv[i + 1];
   }
   if (provider && strcmp(provider, "shared") != 0 && strcmp(provider, "detached") != 0 &&
       strcmp(provider, "mirror") != 0)
      return server_send_error(conn, "workspace: --provider must be shared, detached or mirror",
                               NULL);
   int is_mirror = provider && strcmp(provider, "mirror") == 0;
   int is_detached = provider && strcmp(provider, "detached") == 0;
   if (is_mirror && (!remote || !remote[0]))
      return server_send_error(conn, "workspace: --provider mirror requires --remote <url>", NULL);

   /* A `detached` or `mirror` workspace's root is the CLIENT's path — it does not
    * (and need not) exist on this server's filesystem (the mirror reconstructs a
    * server-side worktree from the remote), so we register it verbatim and skip
    * the local resolve/stat. A `shared` workspace must be a real local directory
    * (resolve + stat), as before. */
   char abs[MAX_PATH_LEN];
   if (is_detached || is_mirror)
   {
      /* The root is the CLIENT's absolute path, never resolved on this server, so
       * accept either POSIX (/...) or Windows (C:\... / C:/...) form — a Windows
       * thin client serving its working tree registers a drive-letter path. */
      const char *r = argv[0];
      int posix_abs = (r[0] == '/');
      int win_abs = (((r[0] >= 'A' && r[0] <= 'Z') || (r[0] >= 'a' && r[0] <= 'z')) &&
                     r[1] == ':' && (r[2] == '\\' || r[2] == '/'));
      if (!posix_abs && !win_abs)
         return server_send_error(conn, "workspace: detached/mirror root must be an absolute path",
                                  NULL);
      snprintf(abs, sizeof(abs), "%s", argv[0]);
   }
   else
   {
      if (!realpath(argv[0], abs))
         return server_send_error(conn, "workspace: cannot resolve path", NULL);
      struct stat st;
      if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode))
         return server_send_error(conn, "workspace: not a directory", NULL);
   }

   /* Mirror workspaces carry the client VCS coordinates the lifecycle seeds from.
    *
    * A DETACHED workspace may carry them too. It is served by its client, so
    * while that client is connected nothing here is used — the live tree is
    * better than any reconstruction of it. But a background delegate runs after
    * the dispatching client has gone, and then the alternative is not a stale
    * tree, it is no tree at all: the turn lands in a scratch directory holding no
    * repository. Recording the coordinates lets that case fall back to the
    * server-side reconstruction instead, which is what
    * workspace_turn_resolve_detached_mirror_cwd exists to do. Nothing could set
    * them before, so that fallback was unreachable. */
   int wants_vcs = is_mirror || is_detached;
   int add_rc = config_workspace_add(abs, provider, wants_vcs ? remote : NULL,
                                     (wants_vcs && head) ? head : NULL);
   /* Already registered is the state the caller asked for, so it is not an
    * error: workspace.add is idempotent. Rejecting the second call made every
    * re-run of any automation that registers its workspace fail at setup,
    * before doing any work -- a benchmark cell re-executed after a harness
    * fault died here in under 90 seconds having accomplished nothing, and the
    * only way to proceed was to hand-edit the registry.
    *
    * Callers cannot avoid this by checking first: between a `workspace list`
    * and the add, another session can register the same path. Idempotence is
    * the only race-free contract. Discovery below still runs, so a repeat call
    * re-discovers projects, which is the useful half on a second call. */
   if (add_rc == -3)
      return server_send_error(conn, "workspace: maximum workspace count reached (64)", NULL);
   if (add_rc != 0 && add_rc != -2)
      return server_send_error(conn, "workspace: failed to save config", NULL);
   int already_registered = (add_rc == -2);

   /* Republish the live snapshot now instead of waiting for the server loop's
    * config_reload_if_changed() tick. In the server, config_load() returns the
    * snapshot rather than disk, so until that tick a `workspace list` issued right
    * after this `workspace add` read a config without the new entry and reported
    * "No workspaces configured" — intermittently, depending on where the write
    * landed in the poll interval. Making the write read-your-writes consistent
    * costs one reload on a rare path. */
   (void)config_reload_if_changed();

   /* A `detached` workspace's root lives on the client; this server cannot
    * enumerate or read it, so we don't discover projects or kick a server-side
    * scan (which would read this host's filesystem). Ingestion is client-driven:
    * the thin client pushes file contents via POST /v1/index/ingest. */
   if (is_detached)
   {
      cJSON *resp = jo_ok();
      jo_add_str(resp, "path", abs);
      jo_add_str(resp, "provider", "detached");
      cJSON_AddArrayToObject(resp, "projects");
      cJSON_AddNumberToObject(resp, "project_count", 0);
      return send_and_free(conn, resp);
   }

   char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
   int count =
       workspace_discover_projects(abs, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
   if (count < 0)
      count = 0;

   /* Registration (config_save above) is all that's strictly required: aimee-kb's
    * background ingest timer reconciles the workspace registry and indexes every
    * discovered project on its own. We only kick an eager scan here when kb is
    * already live, so a down kb never blocks this RPC on a 15s autostart — the
    * background worker picks the projects up once the service is available.
    *
    * The eager scan is a convenience, and on a large tree it is an expensive one:
    * it walks every discovered project before this RPC answers, so registering a
    * 4,000-file repository takes minutes and a caller with a timeout gives up on
    * a registration that already succeeded. `scan: false` registers and returns,
    * leaving the projects to the background ingest timer that would have
    * reconciled them anyway. Default stays true: an interactive
    * `aimee workspace add` should still say whether the index is populated. */
   const cJSON *scan_req = cJSON_GetObjectItemCaseSensitive(req, "scan");
   int eager_scan = cJSON_IsBool(scan_req) ? cJSON_IsTrue(scan_req) : 1;
   int kb_live = eager_scan && kb_client_is_live();
   cJSON *resp = jo_ok();
   jo_add_str(resp, "path", abs);
   /* Idempotent success still tells the caller which it was, so a UI can say
    * "already registered" without having to treat it as a failure. */
   cJSON_AddBoolToObject(resp, "already_registered", already_registered);
   cJSON *arr = cJSON_AddArrayToObject(resp, "projects");
   for (int i = 0; i < count; i++)
   {
      const char *name = strrchr(projects[i], '/');
      name = name ? name + 1 : projects[i];

      cJSON *p = cJSON_CreateObject();
      jo_add_str(p, "name", name);
      jo_add_str(p, "root", projects[i]);

      if (!kb_live)
      {
         cJSON_AddBoolToObject(p, "indexed", 0);
         /* Say which of the two it was. Reporting "knowledge service offline"
          * for a scan the caller asked us to skip would be a false diagnosis of
          * a healthy kb. */
         jo_add_str(p, "reason",
                    eager_scan ? "knowledge service offline — queued for background ingest"
                               : "scan not requested — queued for background ingest");
         cJSON_AddItemToArray(arr, p);
         continue;
      }

      kb_client_index_scan_result_t res;
      memset(&res, 0, sizeof(res));
      int rc = kb_client_index_scan(name, projects[i], 0, &res);

      /* A SCAN THAT VISITED NOTHING IS NOT AN INDEXED PROJECT. kb is handed a
       * filesystem PATH here, and it may not be able to read it -- in the managed
       * topology aimee-server and aimee-kb are separate containers with no shared
       * volume, so a path that exists for the server is absent for kb. kb then
       * walks an empty directory, finds no files, and answers success. rc == 0 and
       * !skipped, so this reported `indexed: true` for a project where nothing was
       * indexed at all: the user is told their repo is in the knowledge base,
       * searches return nothing or another project's files, and no error is
       * printed anywhere.
       *
       * `inspected` is the right test, not `files`: a project already indexed and
       * unchanged legitimately reports files == 0 with inspected > 0, and calling
       * that "not indexed" would be its own wrong answer. inspected == 0 means kb
       * saw no files to consider, which is the failure this distinguishes. Older
       * kb builds do not report inspected (documented as 0), so fall back to files
       * rather than calling a working older kb broken. */
      int ok = server_workspace_scan_indexed(rc, res.skipped, res.inspected, res.files);
      cJSON_AddBoolToObject(p, "indexed", ok);
      if (!ok)
      {
         if (rc != 0 || res.skipped)
            jo_add_str(p, "reason", res.reason[0] ? res.reason : "knowledge service unavailable");
         else
            jo_add_str(p, "reason", WORKSPACE_SCAN_EMPTY_REASON);
      }
      cJSON_AddItemToArray(arr, p);
   }
   cJSON_AddNumberToObject(resp, "project_count", count);
   return send_and_free(conn, resp);
}

/* workspace.mirror-sync: the client serving a `mirror` workspace ships its full
 * working-tree patch vs HEAD (tracked mods + deletions + untracked-file adds, as
 * one `git diff --cached --binary` blob) so the server's reconstructed worktree
 * mirrors the client's working tree (workspace-resource-plane §3). git's --binary
 * patch format is ASCII (base85), so the blob is JSON-safe even for binary files.
 * It is stored at the hashed mirror dir's `client.diff`; the next detached turn's
 * session setup feeds it to workspace_mirror_reconstruct. The diff is the
 * client's own working changes — not a secret — and is written via the provider. */
int handle_workspace_mirror_sync(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char *argv[8];
   int argc = workspace_rpc_args(req, argv, 8);
   if (argc < 1 || !argv[0][0])
      return server_send_error(conn, "usage: workspace mirror-sync <root>", NULL);
   const char *root = argv[0];
   const char *diff = jo_str(req, "diff", "");

   /* A `mirror` workspace always has a server-side tree to mirror. A `detached`
    * one does too once it has recorded a remote — its client serves the live tree
    * while connected, but a background delegate arrives after that client is gone
    * and would otherwise get a scratch directory with no repository in it. Both
    * reconstruct through the same path, so both may sync into it; a workspace
    * with no remote recorded has nothing to reconstruct from and is refused. */
   int syncable = 0, has_remote = 0;
   ws_provider_kind_t kind = WS_PROVIDER_SHARED;
   int ws_n = config_workspace_count();
   for (int i = 0; i < ws_n; i++)
      if (strcmp(config_workspaces(i), root) == 0)
      {
         kind = ws_provider_kind_from_string(config_workspace_providers(i));
         has_remote = config_workspace_vcs_remote(i)[0] != '\0';
         syncable = (kind == WS_PROVIDER_MIRROR) || (kind == WS_PROVIDER_DETACHED && has_remote);
         break;
      }
   if (!syncable)
      return server_send_error(
          conn,
          kind == WS_PROVIDER_DETACHED
              ? "workspace: this detached workspace has no remote recorded, so there is nothing to "
                "reconstruct from. Re-register it with `--remote <url>` (and `--head <sha>`), or "
                "use `--provider mirror`."
              : "workspace: mirror-sync requires a `mirror` workspace, or a `detached` one with a "
                "remote recorded",
          NULL);

   /* The patch and the commit it applies to arrive together, and the registry is
    * updated from the same request. A client's base moves whenever the developer
    * commits or pushes, so accepting the patch while keeping an older head would
    * store a pair that cannot be applied — and the failure would surface later,
    * during a reconstruct, far from the request that caused it. */
   /* A working tree is not bounded by anything the transport can choose, so the
    * patch arrives in chunks and is reassembled here rather than shipped whole.
    * `seq` 0 truncates (discarding a partial from an abandoned run), later ones
    * append, and only `final` commits the head — a head stored against a patch
    * that is still arriving would be applied to a fragment on the next
    * reconstruct, far from the request that caused it.
    *
    * A request with neither field is the single-shot form and behaves exactly as
    * before: seq 0, final true. */
   cJSON *jseq = cJSON_GetObjectItemCaseSensitive(req, "seq");
   cJSON *jfinal = cJSON_GetObjectItemCaseSensitive(req, "final");
   int seq = cJSON_IsNumber(jseq) ? (int)jseq->valuedouble : 0;
   int final = jfinal ? cJSON_IsTrue(jfinal) : 1;
   if (seq < 0)
      return server_send_error(conn, "workspace: mirror-sync seq must not be negative", NULL);

   const char *client_head = jo_str(req, "head", "");
   if (final && client_head && client_head[0])
   {
      /* Keep the registered provider. Hardcoding "mirror" here would silently
       * convert a detached workspace on its first sync, and a detached workspace
       * is detached on purpose: its client serves the live tree, which is better
       * than any reconstruction while that client is connected. The head is
       * recorded for the case where no client is. */
      (void)config_workspace_add(root, ws_provider_kind_to_string(kind), NULL, client_head);
      /* Republish the live snapshot, for the same reason workspace.add does:
       * config_load() serves the snapshot rather than disk in the server, so
       * without this the head sits correct on disk while every reader — the
       * reconstruct included — keeps using the previous one until the server
       * loop's next tick. Measured: the patch shipped against the right base and
       * the checkout still used the stale head, so nothing reconstructed. */
      (void)config_reload_if_changed();
   }

   char base[MAX_PATH_LEN], diff_path[MAX_PATH_LEN];
   if (workspace_mirror_base(base, sizeof(base)) != 0)
      return server_send_error(conn, "workspace: cannot resolve workspaces dir", NULL);
   if (workspace_mirror_diff_path(base, root, diff_path, sizeof(diff_path)) != 0)
      return server_send_error(conn, "workspace: path too long", NULL);

   /* Ensure the hashed parent dir exists (the worktree may not be materialized
    * yet on the first sync), then write the diff via the provider. */
   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", diff_path);
   char *slash = strrchr(parent, '/');
   if (slash)
      *slash = '\0';
   platform_mkdir_p(parent, 0700);

   const workspace_provider_t *ws = workspace_provider_shared();
   if (seq > 0 && !ws->append)
      return server_send_error(
          conn, "workspace: this provider cannot append, so a chunked sync cannot be reassembled",
          NULL);
   int stored = seq == 0 ? ws->write_all(ws, diff_path, diff, strlen(diff))
                         : ws->append(ws, diff_path, diff, strlen(diff));
   if (stored != 0)
      return server_send_error(conn, "workspace: failed to store client diff", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "bytes", (double)strlen(diff));
   cJSON_AddNumberToObject(resp, "seq", seq);
   /* The ack a chunking client checks before sending anything after chunk 0. An
    * older server answers without it, and writes every chunk whole — the last
    * one would win and the mirror would reconstruct from a fragment that looks
    * like a complete tree. The client must be able to tell the difference. */
   cJSON_AddBoolToObject(resp, "chunked", 1);
   return send_and_free(conn, resp);
}

/* runner.poll: the filesystem-authority client serving a detached workspace
 * long-polls for the next op the server needs done against the working tree.
 * Blocks server-side up to ~25s, then returns have_op:false so the client
 * re-polls. */
int handle_runner_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char *argv[8];
   int argc = workspace_rpc_args(req, argv, 8);
   if (argc < 1 || !argv[0][0])
      return server_send_error(conn, "usage: runner.poll <workspace_id>", NULL);

   cJSON *op = ws_runner_registry_poll(argv[0], 25000);
   cJSON *resp = jo_ok();
   jo_add_bool(resp, "have_op", op != NULL);
   if (op)
      cJSON_AddItemToObject(resp, "op", op); /* transfers ownership to resp */
   return send_and_free(conn, resp);
}

/* runner.respond: the client posts the result of the op it just executed back
 * to the server-side transport blocked on this workspace's queue. */
int handle_runner_respond(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char *argv[8];
   int argc = workspace_rpc_args(req, argv, 8);
   if (argc < 1 || !argv[0][0])
      return server_send_error(conn, "usage: runner.respond <workspace_id>", NULL);

   cJSON *r = cJSON_GetObjectItemCaseSensitive(req, "response");
   if (!cJSON_IsObject(r))
      return server_send_error(conn, "runner.respond: missing response object", NULL);

   /* req owns its children; the registry/queue takes ownership of the response,
    * so hand it a copy. */
   cJSON *owned = cJSON_Duplicate(r, 1);
   if (ws_runner_registry_respond(argv[0], owned) != 0)
      return server_send_error(conn, "runner.respond: no runner registered for workspace", NULL);
   return send_and_free(conn, jo_ok());
}

/* index.ingest: a thin client pushes the {"rel_path","content"} contents of a
 * `detached` workspace this server cannot see; relay them to aimee-kb's code
 * scan. SYNCHRONOUS (inline kb push + send_and_free) — unlike index.scan it must
 * not use the kb_proxy_spawn detached-thread path, because /v1/index/ingest is
 * served by the async op-run worker via loopback_rpc (server_http.c), which runs
 * server_dispatch on a socketpair and reads the response synchronously: a reply
 * written later on a detached thread is lost ("rpc produced no response"). The
 * op-run worker runs off the listener thread, so blocking here is fine. */
int handle_index_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = jo_str(req, "name", NULL);
   const char *root = jo_str(req, "root", NULL);
   if (!name || !name[0] || !root || !root[0])
      return server_send_error(conn, "index.ingest requires both name and root", NULL);
   if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(req, "files")))
      return server_send_error(conn, "index.ingest requires a files array", NULL);

   int force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "force")) ? 1 : 0;
   /* Detach the client-pushed array so kb_client_code_scan_push adopts (frees)
    * it without a double-free when req is released. */
   cJSON *files = cJSON_DetachItemFromObjectCaseSensitive(req, "files");

   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   int kb_rc = kb_client_code_scan_push(name, root, force, files, &res);
   cJSON *resp = (cJSON *)kb_client_index_scan_format_response(kb_rc, &res);
   return send_and_free(conn, resp);
}
