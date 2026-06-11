/* server_runner_endpoints.inc: the workspace-handle (workspace.get) and
 * detached-runner reverse-channel (runner.poll/respond) RPC handlers, split out
 * of server_state.c to keep it under the 2000-line limit. Included from
 * server_state.c after workspace_rpc_args, ws_git_line, and the
 * jo_ok/send_and_free helpers it relies on. See workspace-resource-plane §1-3. */

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
      config_t cfg;
      config_load(&cfg);
      for (int i = 0; i < cfg.workspace_count; i++)
         if (strcmp(cfg.workspaces[i], root) == 0)
         {
            if (cfg.workspace_providers[i][0])
               provider = ws_provider_kind_to_string(
                   ws_provider_kind_from_string(cfg.workspace_providers[i]));
            snprintf(reg_remote, sizeof(reg_remote), "%s", cfg.workspace_vcs_remote[i]);
            snprintf(reg_head, sizeof(reg_head), "%s", cfg.workspace_vcs_head[i]);
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

   config_t cfg;
   config_load(&cfg);
   for (int i = 0; i < cfg.workspace_count; i++)
      if (strcmp(cfg.workspaces[i], abs) == 0)
         return server_send_error(conn, "workspace: already registered", NULL);
   if (cfg.workspace_count >= 64)
      return server_send_error(conn, "workspace: maximum workspace count reached (64)", NULL);

   int idx = cfg.workspace_count++;
   snprintf(cfg.workspaces[idx], MAX_PATH_LEN, "%s", abs);
   snprintf(cfg.workspace_providers[idx], sizeof(cfg.workspace_providers[idx]), "%s",
            (provider && strcmp(provider, "shared") != 0) ? provider : "");
   /* Mirror workspaces carry the client VCS coordinates the lifecycle seeds from. */
   snprintf(cfg.workspace_vcs_remote[idx], sizeof(cfg.workspace_vcs_remote[idx]), "%s",
            is_mirror ? remote : "");
   snprintf(cfg.workspace_vcs_head[idx], sizeof(cfg.workspace_vcs_head[idx]), "%s",
            (is_mirror && head) ? head : "");
   if (config_save(&cfg) != 0)
      return server_send_error(conn, "workspace: failed to save config", NULL);

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
    * background worker picks the projects up once the service is available. */
   int kb_live = kb_client_is_live();
   cJSON *resp = jo_ok();
   jo_add_str(resp, "path", abs);
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
         jo_add_str(p, "reason", "knowledge service offline — queued for background ingest");
         cJSON_AddItemToArray(arr, p);
         continue;
      }

      kb_client_index_scan_result_t res;
      memset(&res, 0, sizeof(res));
      int rc = kb_client_index_scan(name, projects[i], 0, &res);
      cJSON_AddBoolToObject(p, "indexed", rc == 0 && !res.skipped);
      if (rc != 0 || res.skipped)
         jo_add_str(p, "reason", res.reason[0] ? res.reason : "knowledge service unavailable");
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

   /* Only a registered `mirror` workspace has a server-side tree to mirror. */
   config_t cfg;
   config_load(&cfg);
   int is_mirror = 0;
   for (int i = 0; i < cfg.workspace_count; i++)
      if (strcmp(cfg.workspaces[i], root) == 0)
      {
         is_mirror = ws_provider_kind_from_string(cfg.workspace_providers[i]) == WS_PROVIDER_MIRROR;
         break;
      }
   if (!is_mirror)
      return server_send_error(
          conn, "workspace: mirror-sync requires a registered `mirror` workspace", NULL);

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
   if (ws->write_all(ws, diff_path, diff, strlen(diff)) != 0)
      return server_send_error(conn, "workspace: failed to store client diff", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "bytes", (double)strlen(diff));
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

/* session.credentials: the thin client pushes its agent API keys (+ Codex OAuth
 * creds) once per session; we cache them in the RAM-only, session-scoped keyring
 * (session_credentials.h) and NEVER persist them. agent_resolve_auth then
 * prefers these over any server-stored key, so agent keys need not live on the
 * server. Body: {session_id, agents:{name:key,...}, codex_oauth_token,
 * codex_account_id}. */
int handle_session_credentials(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *sid = jo_str(req, "session_id", NULL);
   if (!sid || !sid[0])
      return server_send_error(conn, "session.credentials requires session_id", NULL);
   char *body = cJSON_PrintUnformatted(req);
   int n = body ? session_creds_ingest_json(sid, body) : -1;
   free(body);
   if (n < 0)
      return server_send_error(conn, "session.credentials: invalid body", NULL);
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "stored", n);
   return send_and_free(conn, resp);
}
