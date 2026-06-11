/* workspace_turn.c — bind the thread-active resource provider for a turn.
 * See workspace_turn.h. Ties the registered per-workspace provider (config) to
 * the detached provider + its runner queue (registry), via the thread-local
 * active-provider seam the file/exec tools resolve through. */
#include "workspace_turn.h"
#include "config.h"
#include "forge_credentials.h"
#include "log.h"
#include "platform_path.h"
#include "util.h"
#include "workspace_mirror.h"
#include "workspace_provider.h"
#include "workspace_provider_detached.h"
#include "workspace_runner_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

extern char **environ;

/* The detached provider bound for the current turn lives here for the turn's
 * duration (the active-provider pointer aliases &t_turn_detached.base). */
static __thread ws_detached_provider_t t_turn_detached;
static __thread int t_turn_bound;
/* For a `mirror` workspace the turn acts directly on a server-side reconstructed
 * worktree (the `shared` provider), so instead of binding a provider we remap
 * the turn's cwd into that worktree and stash the drift line to surface. */
static __thread char t_turn_cwd[MAX_PATH_LEN];
static __thread char t_turn_drift[256];

/* Is `cwd` inside (or equal to) workspace root `ws`? */
static int cwd_in_workspace(const char *cwd, const char *ws)
{
   size_t len = strlen(ws);
   if (len == 0)
      return 0;
   /* '\\' is also a boundary so a Windows client's subdir (C:\ws\sub) matches its
    * registered root (C:\ws); the exact-match case (cwd[len]=='\0') covers both. */
   return strncmp(cwd, ws, len) == 0 && (cwd[len] == '/' || cwd[len] == '\\' || cwd[len] == '\0');
}

/* Production git runner for the mirror lifecycle: prepend "git" and fork/exec
 * (combined stdout+stderr). `ctx` is the workspace root (a workspace_id); when it
 * has a brokered forge token (§4) — else the server-held identity (§6) — the
 * clone/fetch authenticate under a GH_TOKEN + GIT_ASKPASS-shim env, injected ONLY
 * into the child (never the command line or disk), exactly as mcp_git_run does.
 * No token → the local ops + ambient-cred clone run unchanged via the shared
 * provider's exec seam. The same env is harmlessly present for the local git ops
 * (rev-parse / worktree / apply) too. */
static int ws_mirror_git_runner(void *ctx, const char *const args[], char *out, size_t out_cap)
{
   const char *wsid = (const char *)ctx;
   const char *argv[64];
   int n = 0;
   argv[n++] = "git";
   for (int i = 0; args[i] && n < 63; i++)
      argv[n++] = args[i];
   argv[n] = NULL;

   char **envp = NULL;
   if (wsid && wsid[0])
   {
      envp = forge_cred_build_env(wsid, (long)time(NULL), environ, forge_cred_askpass_shim());
      if (!envp)
         envp = forge_cred_build_server_env(environ, forge_cred_askpass_shim());
   }

   char *cap = NULL;
   int rc;
   if (envp)
   {
      rc = safe_exec_capture_env(argv, envp, &cap, out_cap ? out_cap : 4096);
      forge_cred_free_env(envp);
   }
   else
   {
      const workspace_provider_t *sh = workspace_provider_shared();
      rc = sh->exec(sh, argv, &cap, out_cap ? out_cap : 4096);
   }
   if (out && out_cap)
      snprintf(out, out_cap, "%s", cap ? cap : "");
   free(cap);
   return rc;
}

/* Drive the mirror lifecycle for `root` (the matched workspace) and, on success,
 * remap the turn's cwd into the reconstructed server-side worktree. Returns 1
 * when the turn is bound to the mirror (cwd remapped), 0 to fall through. */
/* Drive the mirror lifecycle for `root` (ensure the bare mirror + reconstruct
 * the server-side worktree) and write the cwd remapped into that worktree into
 * out[out_cap] (the `<root>/sub` suffix is preserved under work_dir). Optional
 * `drift`/`verdict_out` receive the surfaced drift. No thread-local state — used
 * by both the turn binder and the mcp.call git path. Returns 1 on success. */
static int mirror_reconstruct_cwd(const char *cwd, const char *root, const char *remote,
                                  const char *head, char *out, size_t out_cap, char *drift,
                                  size_t drift_cap, ws_mirror_drift_t *verdict_out)
{
   if (out && out_cap)
      out[0] = '\0';
   if (!head || !head[0] || !out || !out_cap)
      return 0; /* without a client head there is nothing to reconstruct */

   char base[MAX_PATH_LEN], mirror_dir[MAX_PATH_LEN], work_dir[MAX_PATH_LEN];
   if (workspace_mirror_base(base, sizeof(base)) != 0)
      return 0; /* durable workspaces dir (AIMEE_WORKSPACES_DIR / aimee_home) unresolvable */
   if (workspace_mirror_paths(base, root, mirror_dir, sizeof(mirror_dir), work_dir,
                              sizeof(work_dir)) != 0)
      return 0;

   /* Ensure the per-workspace parent dir (`<base>/<hash>`) exists before the
    * lifecycle: `git clone --mirror` won't create missing leading dirs, and on a
    * fresh durable volume nothing has created it yet (a prior mirror-sync would
    * have, but this must not depend on that ordering). */
   char hashdir[MAX_PATH_LEN];
   snprintf(hashdir, sizeof(hashdir), "%s", mirror_dir);
   char *slash = strrchr(hashdir, '/');
   if (slash)
      *slash = '\0';
   platform_mkdir_p(hashdir, 0700);

   /* Already materialized? A populated worktree carries a `.git` gitlink file; a
    * resumed session skips the (network) seed + the tree-destroying reconstruct
    * so its in-progress edits survive. */
   char gitmark[MAX_PATH_LEN];
   snprintf(gitmark, sizeof(gitmark), "%s/.git", work_dir);
   struct stat gst;
   int already = (stat(gitmark, &gst) == 0);

   /* Apply the client's shipped uncommitted diff (workspace.mirror-sync) on top
    * of the head checkout, when one is present, so the reconstructed tree mirrors
    * the client's working tree. Absent → a clean checkout at head. */
   char diff_path[MAX_PATH_LEN];
   const char *diff_arg = NULL;
   if (workspace_mirror_diff_path(base, root, diff_path, sizeof(diff_path)) == 0)
   {
      struct stat dst;
      if (stat(diff_path, &dst) == 0 && S_ISREG(dst.st_mode))
         diff_arg = diff_path;
   }

   /* Pass the workspace root as the runner ctx so its git calls authenticate
    * under any brokered/server forge token for this workspace (§4/§6). */
   if (workspace_mirror_session_setup(ws_mirror_git_runner, (void *)root, remote, head, mirror_dir,
                                      work_dir, diff_arg, already, drift, drift_cap,
                                      verdict_out) != 0)
   {
      LOG_WARN("workspace", "mirror lifecycle failed for %s (remote=%s head=%.10s)", root,
               remote ? remote : "", head);
      return 0;
   }

   /* Map the cwd suffix below the workspace root into the reconstructed tree:
    * <root>/src/x  ->  <work_dir>/src/x. */
   const char *suffix = cwd + strlen(root); /* "" or "/..." (cwd_in_workspace guaranteed) */
   int w = snprintf(out, out_cap, "%s%s", work_dir, suffix);
   if (w < 0 || (size_t)w >= out_cap)
   {
      out[0] = '\0';
      return 0;
   }
   return 1;
}

/* Turn binder: drive the lifecycle and stash the remapped cwd + drift line for
 * the turn. Returns 1 when bound (caller must unbind), 0 to fall through. */
static int bind_mirror(const char *cwd, const char *root, const char *remote, const char *head)
{
   ws_mirror_drift_t verdict = WS_MIRROR_DRIFT_UNKNOWN;
   char drift[256] = "";
   if (!mirror_reconstruct_cwd(cwd, root, remote, head, t_turn_cwd, sizeof(t_turn_cwd), drift,
                               sizeof(drift), &verdict))
   {
      t_turn_cwd[0] = '\0';
      return 0; /* fail closed: a remote turn's foreign cwd is rejected upstream */
   }
   if (verdict != WS_MIRROR_IN_SYNC && drift[0])
      snprintf(t_turn_drift, sizeof(t_turn_drift), "%s", drift);
   t_turn_bound = 1;
   return 1;
}

/* Resolve a `mirror`-workspace cwd to its reconstructed server-side worktree cwd
 * (driving the lifecycle), without binding any thread-local turn state. For
 * non-turn callers — e.g. the mcp.call git tools (`/pr` on a mirror workspace) —
 * that need to chdir into the server-side tree. Returns 1 + out when `cwd` is in
 * a registered mirror workspace, 0 otherwise. See workspace_turn.h. */
int workspace_turn_resolve_mirror_cwd(const char *cwd, char *out, size_t out_cap)
{
   if (out && out_cap)
      out[0] = '\0';
   if (!cwd || !cwd[0] || !out || out_cap == 0)
      return 0;

   config_t cfg;
   config_load(&cfg);
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (!cwd_in_workspace(cwd, cfg.workspaces[i]))
         continue;
      ws_provider_kind_t kind = cfg.workspace_providers[i][0]
                                    ? ws_provider_kind_from_string(cfg.workspace_providers[i])
                                    : WS_PROVIDER_SHARED;
      if (kind == WS_PROVIDER_MIRROR)
      {
         ws_mirror_drift_t v;
         return mirror_reconstruct_cwd(cwd, cfg.workspaces[i], cfg.workspace_vcs_remote[i],
                                       cfg.workspace_vcs_head[i], out, out_cap, NULL, 0, &v);
      }
      return 0; /* matched, but not a mirror workspace */
   }
   return 0;
}

int workspace_turn_bind_active(const char *cwd)
{
   t_turn_bound = 0;
   t_turn_cwd[0] = '\0';
   t_turn_drift[0] = '\0';
   if (!cwd || !cwd[0])
      return 0;

   config_t cfg;
   config_load(&cfg);

   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (!cwd_in_workspace(cwd, cfg.workspaces[i]))
         continue;

      /* Matched the workspace containing this turn's cwd. Only a non-shared
       * provider changes anything; shared stays on the direct-fs path. */
      ws_provider_kind_t kind = cfg.workspace_providers[i][0]
                                    ? ws_provider_kind_from_string(cfg.workspace_providers[i])
                                    : WS_PROVIDER_SHARED;
      if (kind == WS_PROVIDER_DETACHED)
      {
         /* The runner queue is keyed by the workspace root — the same id the
          * client's `aimee workspace serve <root>` polls. */
         ws_runner_queue_t *q = ws_runner_registry_get_or_create(cfg.workspaces[i]);
         if (q)
         {
            /* Bind both the unary and streaming transports off the same queue so
             * file/exec ops marshal as today AND a local-CLI agent (exec_stream)
             * can stream its output back from the client. */
            ws_detached_provider_init_ex(&t_turn_detached, ws_runner_queue_transport,
                                         ws_runner_queue_transport_stream, q);
            workspace_provider_set_active(&t_turn_detached.base);
            t_turn_bound = 1;
            return 1;
         }
      }
      else if (kind == WS_PROVIDER_MIRROR)
      {
         /* Drive the server-side mirror lifecycle from the registry's vcs
          * coordinates, then act on the reconstructed local tree (shared fs). */
         return bind_mirror(cwd, cfg.workspaces[i], cfg.workspace_vcs_remote[i],
                            cfg.workspace_vcs_head[i]);
      }
      return 0; /* matched, but shared (or no queue) — stay on shared */
   }
   return 0; /* cwd not in any registered workspace */
}

void workspace_turn_unbind_active(void)
{
   if (t_turn_bound)
   {
      workspace_provider_clear_active(); /* no-op for the mirror path (none bound) */
      t_turn_bound = 0;
   }
   t_turn_cwd[0] = '\0';
   t_turn_drift[0] = '\0';
}

const char *workspace_turn_active_cwd(void)
{
   return t_turn_cwd[0] ? t_turn_cwd : NULL;
}

const char *workspace_turn_drift_notice(void)
{
   return t_turn_drift[0] ? t_turn_drift : NULL;
}

int workspace_turn_reject_foreign_cwd(int detached_bound, int trusted_local, const char *cwd)
{
   /* No cwd, or not an absolute path → the turn runs in the server's own process
    * cwd; there is no foreign path to open, so nothing to reject. */
   if (!cwd || cwd[0] != '/')
      return 0;
   /* A traversal-bearing path is never bound as the cwd anyway (the caller
    * guards `/..`); treat it as "don't bind" rather than a hard reject. */
   if (strstr(cwd, "/.."))
      return 0;
   /* Acted on via the detached provider (on the client), or a co-located peer
    * whose cwd is a real server-side path — both legitimate. */
   if (detached_bound || trusted_local)
      return 0;
   /* Remote peer + raw foreign absolute path + no detached workspace: the
    * server must not open it on its own filesystem. Reject. */
   return 1;
}
