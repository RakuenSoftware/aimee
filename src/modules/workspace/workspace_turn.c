/* workspace_turn.c — bind the thread-active resource provider for a turn.
 * See workspace_turn.h. Ties the registered per-workspace provider (config) to
 * the detached provider + its runner queue (registry), via the thread-local
 * active-provider seam the file/exec tools resolve through. */
#include "workspace_turn.h"
#include "aimee_sha256.h"
#include "platform_random.h"
#include "aimee_home.h" /* aimee_home — authorize aimee's own managed worktrees */
#include "config.h"
#include "modules/git/forge_credentials.h"
#include "modules/git/git_cred_inject.h"
#include "log.h"
#include "platform_path.h"
#include "util.h"
#include "workspace_mirror.h"
#include "workspace_provider.h"
#include "workspace_provider_container.h"
#include "workspace_provider_detached.h"
#include "workspace_runner_registry.h"
#include <aimee/workspace/module_api.h>

/* The server's request context, absent from binaries that link this module
 * without it (then every op goes to the tree's own runner). */
const char *request_context_runner_channel(void) __attribute__((weak));

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

extern char **environ;

static workspace_turn_project_open_fn project_owner;

void workspace_turn_register_project_owner(workspace_turn_project_open_fn owner)
{
   project_owner = owner;
}

/* The detached provider bound for the current turn lives here for the turn's
 * duration (the active-provider pointer aliases &t_turn_detached.base). */
static __thread ws_detached_provider_t t_turn_detached;
/* A runner channel's queue handle, owned by the binding that uses it. Channels
 * are per command, so they cannot take one of the registry's fixed handles. */
static __thread ws_runner_queue_t t_turn_channel;
static __thread int t_turn_bound;
static __thread int t_turn_mirror_fd = -1;
static __thread char t_turn_detached_epoch[65], t_turn_container_epoch[65];
/* Same, for a delegate turn bound to its own container. Held separately from
 * t_turn_detached because this one owns a CONTAINER: unbinding has to release it,
 * not just drop the pointer, or every delegate turn leaks a container. */
static __thread ws_container_provider_t t_turn_container;
static __thread delegate_backend_t *t_turn_container_backend;
static __thread void *t_turn_container_state;
static __thread int t_turn_container_hibernate;
/* Host source custody is separate from the container file/exec provider. */
static __thread int t_turn_container_source_fd = -1;
static __thread char t_turn_container_source_cwd[MAX_PATH_LEN];
/* For a `mirror` workspace the turn acts directly on a server-side reconstructed
 * worktree (the `shared` provider), so instead of binding a provider we remap
 * the turn's cwd into that worktree and stash the drift line to surface. */
static __thread char t_turn_cwd[MAX_PATH_LEN];
static __thread char t_turn_drift[256];
static __thread char t_test_default_workspace[MAX_PATH_LEN];

struct workspace_turn_mirror_lease
{
   int fd;
   char cwd[MAX_PATH_LEN];
};
static __thread const workspace_turn_mirror_lease_t *t_callback_mirror;

struct workspace_turn_scope
{
   ws_detached_provider_t detached;
   ws_runner_queue_t channel;
   int bound;
   int mirror_fd, created_fd;
   char created_cwd[MAX_PATH_LEN];
   char epoch[65];
   char cwd[MAX_PATH_LEN], drift[256];
   const workspace_provider_t *previous_provider;
   struct workspace_turn_scope *previous;
};
static __thread workspace_turn_scope_t *t_nested_scope;

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

/* The mirror runner needs the workspace's remote URL for the per-host vault
 * lookup. It used to carry a workspace_id as well, purely as the broker key;
 * that went with the broker token itself. */
typedef struct
{
   const char *remote; /* the workspace's vcs remote URL, or NULL */
} ws_mirror_runner_ctx_t;

/* Production git runner for the mirror lifecycle: prepend "git" and fork/exec
 * (combined stdout+stderr). Credentials are resolved through the one shared
 * vault-first policy: the per-host vault token for the remote's host, else the
 * principal's vaulted forge token, else the server identity (§6), injected ONLY
 * into the child (never the command line or disk), exactly as mcp_git_run does.
 * No credential → the local ops + ambient-cred clone run unchanged via the
 * shared provider's exec seam. The same env is harmlessly present for the local
 * git ops (rev-parse / worktree / apply) too. */
static int ws_mirror_git_runner(void *ctx, const char *const args[], char *out, size_t out_cap)
{
   const ws_mirror_runner_ctx_t *rctx = (const ws_mirror_runner_ctx_t *)ctx;
   const char *remote = rctx ? rctx->remote : NULL;
   const char *argv[64];
   int n = 0;
   argv[n++] = "git";
   for (int i = 0; args[i] && n < 63; i++)
      argv[n++] = args[i];
   argv[n] = NULL;

   /* Resolve the git credential through the ONE policy
    * (git_cred_inject_build_env_for_repo) so the precedence never drifts from the
    * other call sites: per-host server vault → the principal's vaulted forge
    * token → server identity → ambient. The policy injects GH_TOKEN + the
    * GIT_ASKPASS shim and wipes its own token copy.
    *
    * NO WORKSPACE BROKER TOKEN, by operator ruling: aimee git proxies through
    * aimee's OWN vaulted credential. A brokered token passed as preferred_token
    * outranks the vault, so this path would authenticate as something weaker
    * than the in-process forge calls that pass none — the same repository, two
    * identities, and a push refused for an account that has admin. */
   int token_fd = -1;
   char **envp = git_cred_inject_build_env_for_repo(NULL, remote, NULL, NULL, environ, &token_fd);

   char *cap = NULL;
   int rc;
   if (envp)
   {
      /* FD mode: the token rides an inherited memfd placed at
       * GIT_CRED_TOKEN_TARGET_FD, so it never lands in the child's environ. */
      rc = safe_exec_capture_cwd_env_fd_timeout(argv, NULL, envp, &cap, out_cap ? out_cap : 4096,
                                                GIT_NET_TIMEOUT_MS, token_fd,
                                                token_fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1);
      git_cred_inject_free_env(envp);
   }
   else
   {
      /* Ambient credentials are allowed for a co-located development server,
       * but ambient prompting is not.  Reuse the hardened network runner so a
       * missing token, SSH passphrase, or unreachable forge cannot pin an HTTP
       * worker indefinitely while it materializes a mirror. */
      rc = git_net_exec(NULL, args, &cap, out_cap ? out_cap : 4096);
   }
   if (token_fd >= 0)
      close(token_fd);
   if (out && out_cap)
      snprintf(out, out_cap, "%s%s", cap ? cap : "",
               rc == SAFE_EXEC_TIMEOUT ? "git command timed out after 30 seconds\n" : "");
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
   {
      /* Every `return 0` below means "this mirror could not be resolved", and
       * each one used to be silent. The caller then left the client-side path
       * in place, which does not exist server-side, and the request fell
       * through to the workspace RUNNER — where it waited out WS_RUNNER_OP_MS
       * (600s) and failed with "unavailable through the registered workspace
       * runner".
       *
       * So the operator saw a ten-minute stall and an error naming a subsystem
       * that was never the problem, with NOTHING in the log: no clone, no
       * fetch, no warning, no trace of the decision that actually caused it.
       * Diagnosing it took hours of black-box probing and produced three wrong
       * theories (a server deadlock, a stale session, missing credentials)
       * before the real shape appeared. A resolver that fails invisibly and
       * blames its fallback is the defect; the log line is the fix. */
      LOG_WARN("workspace", "mirror resolve: no client head for %s — nothing to reconstruct",
               root ? root : "(null)");
      return 0;
   }

   char base[MAX_PATH_LEN], mirror_dir[MAX_PATH_LEN], work_dir[MAX_PATH_LEN];
   if (workspace_mirror_base(base, sizeof(base)) != 0)
   {
      LOG_WARN("workspace",
               "mirror resolve: workspaces dir unresolvable (AIMEE_WORKSPACES_DIR / aimee_home) "
               "for %s",
               root);
      return 0;
   }
   if (workspace_mirror_paths(base, root, mirror_dir, sizeof(mirror_dir), work_dir,
                              sizeof(work_dir)) != 0)
   {
      LOG_WARN("workspace", "mirror resolve: cannot derive mirror/work paths under %s for %s", base,
               root);
      return 0;
   }

   /* mirror-sync publishes immutable, generation-qualified snapshots.  Prefer
    * that atomically-published metadata over the legacy mutable client.diff so
    * a changed client checkout gets a fresh worktree while repeated Git calls
    * keep using (and preserving) the same server-side session. */
   char effective_head[65];
   snprintf(effective_head, sizeof(effective_head), "%s", head);
   char effective_branch[256] = "", effective_upstream[256] = "";
   char snapshot_meta[MAX_PATH_LEN], snapshot_diff[MAX_PATH_LEN];
   snapshot_diff[0] = '\0';
   if (workspace_mirror_snapshot_meta_path(base, root, snapshot_meta, sizeof(snapshot_meta)) != 0)
   {
      LOG_WARN("workspace", "mirror resolve: cannot derive the snapshot metadata path for %s",
               root);
      return 0;
   }

   /* Absence means this workspace predates snapshot publication and may use the
    * legacy head/diff pair. Once metadata exists it is authoritative: unreadable
    * or malformed metadata must fail closed, not silently revive stale state. */
   {
      const workspace_provider_t *shared = workspace_provider_shared();
      ws_stat_t meta_stat;
      if (shared->stat(shared, snapshot_meta, &meta_stat) != 0)
      {
         LOG_WARN("workspace", "mirror resolve: cannot stat snapshot metadata %s for %s",
                  snapshot_meta, root);
         return 0;
      }
      if (meta_stat.exists)
      {
         if (meta_stat.is_dir)
         {
            LOG_WARN("workspace", "mirror resolve: snapshot metadata path %s is a directory",
                     snapshot_meta);
            return 0;
         }
         char *metadata = NULL;
         size_t metadata_len = 0;
         ws_mirror_snapshot_t snapshot;
         int valid =
             shared->read_all(shared, snapshot_meta, &metadata, &metadata_len) == 0 &&
             workspace_mirror_snapshot_parse(metadata, &snapshot) == 0 &&
             workspace_mirror_snapshot_work_path(base, root, snapshot.generation, snapshot.digest,
                                                 work_dir, sizeof(work_dir)) == 0 &&
             workspace_mirror_snapshot_diff_path(base, root, snapshot.generation, snapshot.digest,
                                                 snapshot_diff, sizeof(snapshot_diff)) == 0;
         if (valid)
         {
            snprintf(effective_head, sizeof(effective_head), "%s", snapshot.head);
            snprintf(effective_branch, sizeof(effective_branch), "%s", snapshot.branch);
            snprintf(effective_upstream, sizeof(effective_upstream), "%s", snapshot.upstream);
         }
         free(metadata);
         if (!valid)
         {
            LOG_WARN("workspace",
                     "client snapshot metadata is invalid for %s; refusing legacy fallback", root);
            return 0;
         }
      }
   }

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
   if (snapshot_diff[0])
      snprintf(diff_path, sizeof(diff_path), "%s", snapshot_diff);
   else if (workspace_mirror_diff_path(base, root, diff_path, sizeof(diff_path)) != 0)
      diff_path[0] = '\0';
   if (diff_path[0])
   {
      struct stat dst;
      if (stat(diff_path, &dst) == 0 && S_ISREG(dst.st_mode))
         diff_arg = diff_path;
   }

   /* Pass the workspace root + remote as the runner ctx so its git calls
    * authenticate under the brokered token, else the per-host vault token for the
    * remote's host, else the server forge identity (§4/§6). */
   ws_mirror_runner_ctx_t rctx = {.remote = remote};
   if (workspace_mirror_session_setup_branch(ws_mirror_git_runner, &rctx, remote, effective_head,
                                             effective_branch, effective_upstream, mirror_dir,
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
   /* A repository symlink cannot turn a client suffix into host authority. */
   char root_real[MAX_PATH_LEN], cwd_real[MAX_PATH_LEN];
   if (!realpath(work_dir, root_real) || !realpath(out, cwd_real) ||
       !cwd_in_workspace(cwd_real, root_real))
   {
      out[0] = 0;
      return 0;
   }
   if (snprintf(out, out_cap, "%s", cwd_real) >= (int)out_cap)
   {
      out[0] = 0;
      return 0;
   }
   return 1;
}

/* Turn binder: drive the lifecycle and stash the remapped cwd + drift line for
 * the turn. Returns 1 when bound (caller must unbind), 0 to fall through. */
static int bind_mirror(const char *cwd, const char *root, const char *remote, const char *head,
                       char resolved[MAX_PATH_LEN], char notice[256])
{
   ws_mirror_drift_t verdict = WS_MIRROR_DRIFT_UNKNOWN;
   char drift[256] = "";
   if (!mirror_reconstruct_cwd(cwd, root, remote, head, resolved, MAX_PATH_LEN, drift,
                               sizeof(drift), &verdict))
   {
      resolved[0] = '\0';
      return 0; /* fail closed: a remote turn's foreign cwd is rejected upstream */
   }
   if (verdict != WS_MIRROR_IN_SYNC && drift[0])
      snprintf(notice, 256, "%s", drift);
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
   for (int i = 0; i < config_workspace_count(); i++)
   {
      if (!cwd_in_workspace(cwd, config_workspaces(i)))
         continue;
      ws_provider_kind_t kind = config_workspace_providers(i)[0]
                                    ? ws_provider_kind_from_string(config_workspace_providers(i))
                                    : WS_PROVIDER_SHARED;
      if (kind == WS_PROVIDER_MIRROR)
      {
         ws_mirror_drift_t v;
         /* Copied out of the accessor buffers: mirror_reconstruct_cwd stores root
          * and remote in a ws_mirror_runner_ctx_t and drives a git-runner callback
          * chain with it, so a config read anywhere under there would move them. */
         char root[MAX_PATH_LEN], remote[512], head[65];
         snprintf(root, sizeof(root), "%s", config_workspaces(i));
         snprintf(remote, sizeof(remote), "%s", config_workspace_vcs_remote(i));
         snprintf(head, sizeof(head), "%s", config_workspace_vcs_head(i));
         int resolved = mirror_reconstruct_cwd(cwd, root, remote, head, out, out_cap, NULL, 0, &v);
         if (!resolved)
            LOG_WARN("workspace",
                     "mirror resolve FAILED for registered workspace %s (cwd=%s); the caller will "
                     "fall back to the runner path and report a runner error after its timeout",
                     root, cwd);
         return resolved;
      }
      return 0; /* matched, but not a mirror workspace */
   }
   /* Not in any registered workspace. Worth saying: a session worktree is
    * expected to be registered, and "not registered" and "registered but
    * unresolvable" produce the SAME downstream error, which is what made this
    * hard to tell apart from the outside. */
   LOG_INFO("workspace", "mirror resolve: %s is not inside any registered workspace", cwd);
   return 0;
}

static int active_mirror_lease(const char *real);

static int bind_active_into(const char *cwd, ws_detached_provider_t *detached,
                            ws_runner_queue_t *channel_slot, int *bound, int *mirror_fd,
                            char resolved[MAX_PATH_LEN], char drift[256], int background)
{
   *bound = 0;
   *mirror_fd = -1;
   resolved[0] = '\0';
   drift[0] = '\0';
   if (!cwd || !cwd[0])
      return 0;
   /* Nested foreground hooks/tools and background workers retain the same
    * producer-held mirror capability. A pathname alone still grants nothing. */
   {
      char actual[MAX_PATH_LEN];
      int source = realpath(cwd, actual) ? active_mirror_lease(actual) : -1;
      if (source >= 0)
      {
         *mirror_fd = fcntl(source, F_DUPFD_CLOEXEC, 3);
         if (*mirror_fd < 0)
            return -1;
         snprintf(resolved, MAX_PATH_LEN, "%s", actual);
         *bound = 1;
         workspace_provider_set_active(workspace_provider_shared());
         return 1;
      }
   }
   for (int i = 0; i < config_workspace_count(); i++)
   {
      if (!cwd_in_workspace(cwd, config_workspaces(i)))
         continue;

      /* Matched the workspace containing this turn's cwd. Only a non-shared
       * provider changes anything; shared stays on the direct-fs path. */
      ws_provider_kind_t kind = config_workspace_providers(i)[0]
                                    ? ws_provider_kind_from_string(config_workspace_providers(i))
                                    : WS_PROVIDER_SHARED;
      if (kind == WS_PROVIDER_DETACHED && !background)
      {
         /* The runner queue is the requesting CLI's own channel when it sent
          * one, so the client that asked runs the op -- not whichever client
          * polled the tree first, with its own environment and credentials.
          * Otherwise it is keyed by the workspace root, the id `aimee
          * workspace serve <root>` polls. */
         const char *channel =
             request_context_runner_channel ? request_context_runner_channel() : "";
         ws_runner_queue_t *q = NULL;
         if (aimee_ws_runner_channel_valid(channel))
         {
            /* The binding owns the channel's handle: the registry's handles
             * are a fixed table for trees, and one per command would run out. */
            snprintf(channel_slot->id, sizeof(channel_slot->id), "%s", channel);
            q = ws_runner_registry_announce(channel) == 0 ? channel_slot : NULL;
         }
         else
            q = ws_runner_registry_get_or_create(config_workspaces(i));
         if (q)
         {
            /* Bind both the unary and streaming transports off the same queue so
             * file/exec ops marshal as today AND a local-CLI agent (exec_stream)
             * can stream its output back from the client. */
            ws_detached_provider_init_ex(detached, ws_runner_queue_transport,
                                         ws_runner_queue_transport_stream, q);
            workspace_provider_set_active(&detached->base);
            *bound = 1;
            return 1;
         }
      }
      else if (kind == WS_PROVIDER_MIRROR || (background && kind == WS_PROVIDER_DETACHED))
      {
         /* Drive the server-side mirror lifecycle from the registry's vcs
          * coordinates, then act on the reconstructed local tree (shared fs). */
         /* Same copy-out as the resolver above: these reach the git runner. */
         char root[MAX_PATH_LEN], remote[512], head[65];
         snprintf(root, sizeof(root), "%s", config_workspaces(i));
         snprintf(remote, sizeof(remote), "%s", config_workspace_vcs_remote(i));
         snprintf(head, sizeof(head), "%s", config_workspace_vcs_head(i));
         *bound = bind_mirror(cwd, root, remote, head, resolved, drift);
         if (*bound &&
             (*mirror_fd = open(resolved, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
         {
            *bound = 0;
            resolved[0] = 0;
            return -1;
         }
         if (*bound)
            workspace_provider_set_active(workspace_provider_shared());
         return *bound ? 1 : -1;
      }
      return 0; /* matched, but shared (or no queue) — stay on shared */
   }
   return 0; /* cwd not in any registered workspace */
}

int workspace_turn_bind_active(const char *cwd)
{
   if (t_turn_mirror_fd >= 0)
      close(t_turn_mirror_fd);
   int result = bind_active_into(cwd, &t_turn_detached, &t_turn_channel, &t_turn_bound,
                                 &t_turn_mirror_fd, t_turn_cwd, t_turn_drift, 0);
   t_turn_detached_epoch[0] = 0;
   if (result > 0 && workspace_provider_active() == &t_turn_detached.base &&
       platform_random_hex(t_turn_detached_epoch, 64))
   {
      workspace_provider_clear_active();
      t_turn_bound = 0;
      return -1;
   }
   return result;
}
static int scope_begin(const char *cwd, workspace_turn_scope_t **out, int background)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!cwd || !cwd[0])
      return 0;
   workspace_turn_scope_t *scope = calloc(1, sizeof *scope);
   if (!scope)
      return -1;
   scope->created_fd = -1;
   scope->previous_provider = workspace_provider_active();
   scope->previous = t_nested_scope;
   int result = bind_active_into(cwd, &scope->detached, &scope->channel, &scope->bound,
                                 &scope->mirror_fd, scope->cwd, scope->drift, background);
   if (result < 0)
   {
      workspace_provider_set_active(scope->previous_provider);
      if (scope->mirror_fd >= 0)
         close(scope->mirror_fd);
      free(scope);
      return -1;
   }
   if (result > 0 && workspace_provider_active() == &scope->detached.base &&
       platform_random_hex(scope->epoch, 64))
   {
      workspace_provider_set_active(scope->previous_provider);
      free(scope);
      return -1;
   }
   t_nested_scope = scope;
   *out = scope;
   return result;
}
int workspace_turn_scope_begin(const char *cwd, workspace_turn_scope_t **out)
{
   return scope_begin(cwd, out, 0);
}
int workspace_turn_scope_begin_background(const char *cwd, workspace_turn_scope_t **out)
{
   return scope_begin(cwd, out, 1);
}
int workspace_turn_scope_end(workspace_turn_scope_t *scope)
{
   if (!scope)
      return 0;
   if (scope != t_nested_scope)
      return -1;
   workspace_provider_set_active(scope->previous_provider);
   t_nested_scope = scope->previous;
   if (scope->mirror_fd >= 0)
      close(scope->mirror_fd);
   if (scope->created_fd >= 0)
      close(scope->created_fd);
   free(scope);
   return 0;
}

const char *workspace_turn_git_target(const char *tool, const char *path, const char *cwd)
{
   if ((!tool || strcmp(tool, "git_clone") != 0) && path && path[0])
      return path;
   return (cwd && cwd[0]) ? cwd : NULL;
}

/* Only the actively reconstructed directory is a mirror lease. A held inode
 * prevents a replaced path/symlink from becoming a new filesystem grant. */
static int mirror_lease_matches(const char *real, const char *path, int fd)
{
   char bound[MAX_PATH_LEN];
   struct stat held, current;
   return fd >= 0 && path[0] && realpath(path, bound) && strcmp(bound, "/") &&
          !strcmp(real, bound) && !fstat(fd, &held) && !stat(bound, &current) &&
          S_ISDIR(held.st_mode) && held.st_dev == current.st_dev && held.st_ino == current.st_ino;
}
static int active_mirror_lease(const char *real)
{
   if (t_callback_mirror &&
       mirror_lease_matches(real, t_callback_mirror->cwd, t_callback_mirror->fd))
      return t_callback_mirror->fd;
   for (workspace_turn_scope_t *scope = t_nested_scope; scope; scope = scope->previous)
   {
      if (mirror_lease_matches(real, scope->created_cwd, scope->created_fd))
         return scope->created_fd;
      if (mirror_lease_matches(real, scope->cwd, scope->mirror_fd))
         return scope->mirror_fd;
   }
   return mirror_lease_matches(real, t_turn_cwd, t_turn_mirror_fd) ? t_turn_mirror_fd : -1;
}

/* Git's common directory binds a linked checkout to the held source repository.
 * Inherited GIT_DIR/GIT_WORK_TREE/config cannot supply that identity. */
static int mirror_repository(const char *cwd, char out[MAX_PATH_LEN])
{
   const char *path = getenv("PATH");
   if (!path)
      path = "/usr/bin:/bin";
   size_t size = strlen(path) + 6;
   char *path_env = malloc(size);
   if (!path_env)
      return 0;
   snprintf(path_env, size, "PATH=%s", path);
   char *environment[] = {path_env,
                          "GIT_CONFIG_NOSYSTEM=1",
                          "GIT_CONFIG_GLOBAL=/dev/null",
                          "GIT_OPTIONAL_LOCKS=0",
                          "GIT_TERMINAL_PROMPT=0",
                          NULL};
   const char *argv[] = {
       "git", "-C", cwd, "rev-parse", "--path-format=absolute", "--git-common-dir", NULL};
   char *result = NULL;
   int rc =
       safe_exec_capture_cwd_env_timeout(argv, NULL, environment, &result, MAX_PATH_LEN + 1, 5000);
   if (result)
      result[strcspn(result, "\r\n")] = 0;
   int valid = !rc && result && result[0] && realpath(result, out) && strcmp(out, "/");
   free(result);
   free(path_env);
   return valid;
}

int workspace_turn_scope_hold_created_worktree(workspace_turn_scope_t *scope, const char *cwd)
{
   if (!scope || scope != t_nested_scope)
      return -1;
   if (scope->mirror_fd < 0)
      return 0; /* Ordinary registered shared trees retain their existing authority. */
   char actual[MAX_PATH_LEN], repository[MAX_PATH_LEN], child_repository[MAX_PATH_LEN];
   struct stat before, held;
   if (scope->created_fd >= 0 || !cwd || !realpath(cwd, actual) ||
       !mirror_lease_matches(scope->cwd, scope->cwd, scope->mirror_fd) ||
       !strcmp(actual, scope->cwd) || lstat(cwd, &before) || !S_ISDIR(before.st_mode) ||
       !mirror_repository(scope->cwd, repository) || !mirror_repository(actual, child_repository) ||
       strcmp(repository, child_repository))
      return -1;
   int fd = open(cwd, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return -1;
   if (fstat(fd, &held) || before.st_dev != held.st_dev || before.st_ino != held.st_ino ||
       !mirror_lease_matches(actual, cwd, fd) ||
       !mirror_lease_matches(scope->cwd, scope->cwd, scope->mirror_fd))
   {
      close(fd);
      return -1;
   }
   scope->created_fd = fd;
   snprintf(scope->created_cwd, sizeof scope->created_cwd, "%s", actual);
   return 0;
}

int workspace_turn_mirror_capture(const char *cwd, workspace_turn_mirror_lease_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   char actual[MAX_PATH_LEN];
   if (!cwd || !cwd[0] || !realpath(cwd, actual))
      return 0;
   int source = active_mirror_lease(actual);
   if (source < 0)
      return 0;
   workspace_turn_mirror_lease_t *lease = calloc(1, sizeof *lease);
   if (!lease)
      return -1;
   lease->fd = fcntl(source, F_DUPFD_CLOEXEC, 3);
   if (lease->fd < 0)
   {
      free(lease);
      return -1;
   }
   snprintf(lease->cwd, sizeof lease->cwd, "%s", actual);
   *out = lease;
   return 0;
}
int workspace_turn_mirror_valid(const workspace_turn_mirror_lease_t *lease, const char *cwd)
{
   char actual[MAX_PATH_LEN];
   return lease && cwd && realpath(cwd, actual) &&
          mirror_lease_matches(actual, lease->cwd, lease->fd);
}
const workspace_turn_mirror_lease_t *
workspace_turn_mirror_bind(const workspace_turn_mirror_lease_t *lease)
{
   const workspace_turn_mirror_lease_t *previous = t_callback_mirror;
   t_callback_mirror = lease;
   return previous;
}
void workspace_turn_mirror_release(workspace_turn_mirror_lease_t *lease)
{
   if (!lease)
      return;
   close(lease->fd);
   free(lease);
}

/* Is `workspace` a tree aimee may hand a delegate? Canonicalizes into `out` and
 * returns 1 if it lives inside one of the operator's REGISTERED workspace roots.
 *
 * Repository-ness is NOT authorization: `mkdir .git` in any directory satisfies
 * that, so the backend's own .git check is a backstop, not the bound. The bound is
 * the registered roots — the same list workspace_turn_bind_active resolves a turn's
 * cwd against. A path outside them is not a workspace the operator ever offered
 * aimee, and bind-mounting it into a delegate container would expose whatever it
 * happens to be: a home directory, a secrets tree, `/`.
 *
 * Canonicalize first: the check and the mount must be the same path, and both
 * stat() and the docker daemon follow symlinks. */
int workspace_turn_workspace_authorized(const char *workspace, char *out, size_t out_cap)
{
   if (out && out_cap)
      out[0] = '\0';
   if (!workspace || !workspace[0] || !out || out_cap == 0)
      return 0;
   char real[MAX_PATH_LEN];
   if (!realpath(workspace, real))
   {
      LOG_ERROR("delegate-sandbox", "workspace '%s' does not resolve (%s)", workspace,
                strerror(errno));
      return 0;
   }
   if (active_mirror_lease(real) >= 0)
   {
      snprintf(out, out_cap, "%s", real);
      return 1;
   }
   /* Aimee's OWN managed worktrees — a WFE per-slice tree under
    * $AIMEE_HOME/wfe-worktrees, or a session worktree under $AIMEE_HOME/.aimee/
    * worktrees — are isolation trees aimee itself created to hand a delegate, not an
    * operator directory that must be pre-registered. Authorize a path that resolves
    * under one of those aimee-owned roots (checked against the canonical AIMEE_HOME,
    * so an unrelated operator path merely CONTAINING the marker is not authorized),
    * so a write-capable WFE slice delegate runs in a container instead of falling
    * back to the in-process host path. */
   {
      const char *home = aimee_home();
      char home_real[MAX_PATH_LEN];
      if (home && home[0] && realpath(home, home_real))
      {
         static const char *const aimee_roots[] = {"wfe-worktrees", ".aimee/worktrees"};
         for (size_t i = 0; i < sizeof(aimee_roots) / sizeof(aimee_roots[0]); i++)
         {
            char root[MAX_PATH_LEN];
            snprintf(root, sizeof(root), "%s/%s", home_real, aimee_roots[i]);
            if (cwd_in_workspace(real, root))
            {
               snprintf(out, out_cap, "%s", real);
               return 1;
            }
         }
      }
   }
   if (project_owner)
   {
      int fd = project_owner(real);
      if (fd >= 0)
      {
         struct stat held, named;
         int valid = !fstat(fd, &held) && !stat(real, &named) && held.st_dev == named.st_dev &&
                     held.st_ino == named.st_ino;
         close(fd);
         if (valid)
         {
            snprintf(out, out_cap, "%s", real);
            return 1;
         }
      }
   }
   for (int i = 0; i < config_workspace_count(); i++)
   {
      const char *provider = config_workspace_providers(i);
      ws_provider_kind_t kind =
          provider[0] ? ws_provider_kind_from_string(provider) : WS_PROVIDER_SHARED;
      /* These roots belong to the client. Only a held server reconstruction
       * above can authorize their filesystem replacement. */
      if (kind == WS_PROVIDER_MIRROR || kind == WS_PROVIDER_DETACHED)
         continue;
      char root_real[MAX_PATH_LEN];
      if (!realpath(config_workspaces(i), root_real))
         continue;
      /* "/" as a registered root would authorize every path on the host, which is
       * not a workspace registration — it is the absence of one. */
      if (strcmp(root_real, "/") == 0)
      {
         LOG_WARN("delegate-sandbox",
                  "ignoring registered workspace root '/' for sandbox authorization: it would "
                  "authorize every directory on the host");
         continue;
      }
      if (cwd_in_workspace(real, root_real))
      {
         snprintf(out, out_cap, "%s", real);
         return 1;
      }
   }
   LOG_ERROR("delegate-sandbox",
             "refusing to mount '%s' — it is not inside any registered workspace root (%d "
             "registered). A directory the operator never registered is not a tree aimee may "
             "hand a delegate",
             real, config_workspace_count());
   return 0;
}

int workspace_turn_bind_container(const char *task_id, const char *image, const char *workspace,
                                  int workspace_read_only)
{
   if (!task_id || !task_id[0])
      return 0;
   if (t_turn_container_backend || t_turn_container_source_fd >= 0)
      return -1; /* Never overwrite a live source/provider owner. */
   /* A delegate IS a sandboxed container. There is no second execution model to
    * fall back to, so every failure below refuses (-1) rather than returning 0.
    *
    * The 0 return used to mean "run this delegate in-process, inside aimee-server,
    * with the server's filesystem and environment". That was an un-isolated host
    * path living beside the container one, and every way the sandbox could fail to
    * apply grew a branch back to it. Refusing is the whole point: a delegate that
    * cannot be sandboxed does not run. */

   if ((!workspace || !workspace[0]) && t_test_default_workspace[0])
      workspace = t_test_default_workspace;
   char ws_real[MAX_PATH_LEN] = "";
   if (workspace && workspace[0] &&
       !workspace_turn_workspace_authorized(workspace, ws_real, sizeof(ws_real)))
      return -1; /* refused + logged by the authorization check */
   if (!ws_real[0])
   {
      LOG_ERROR("delegate-sandbox",
                "delegate '%s': a complete registered source workspace is required; refusing an "
                "empty sandbox",
                task_id);
      return -1;
   }

   int source_fd = open(ws_real, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   struct stat source_status;
   if (source_fd < 0 || fstat(source_fd, &source_status))
   {
      if (source_fd >= 0)
         close(source_fd);
      return -1;
   }
   delegate_backend_t *b = delegate_backend_lookup("docker");
   if (!b || !b->acquire)
   {
      LOG_ERROR("delegate-sandbox",
                "delegate '%s': the docker backend is unavailable — refusing to run. A delegate "
                "runs in a container or not at all",
                task_id);
      close(source_fd);
      return -1;
   }

   delegate_backend_config_t bcfg = {
       .image = (image && image[0]) ? image : NULL,
       .host = NULL,
       .hibernate_on_exit = 0,
       .workspace = ws_real[0] ? ws_real : NULL,
       .workspace_read_only = workspace_read_only,
       .pkg_proxy = (strcmp(config_delegate_sandbox_package_access(), "proxy") == 0),
       .require_isolation = 1};
   void *state = NULL;
   int arc = b->acquire(b, task_id, &bcfg, &state);
   if (arc == DELEGATE_ACQUIRE_REFUSED_ISOLATION)
   {
      LOG_ERROR("delegate-sandbox",
                "delegate '%s': refusing to run — the runtime did not provide network isolation",
                task_id);
      close(source_fd);
      return -1;
   }
   if (arc != 0 || !state)
   {
      LOG_ERROR("delegate-sandbox",
                "delegate '%s': could not acquire a container (image=%s) — refusing to run",
                task_id, bcfg.image ? bcfg.image : "<default>");
      close(source_fd);
      return -1;
   }

   struct stat current;
   if (stat(ws_real, &current) || current.st_dev != source_status.st_dev ||
       current.st_ino != source_status.st_ino || !S_ISDIR(current.st_mode))
   {
      b->release(b, state, 0);
      close(source_fd);
      return -1;
   }
   if (ws_container_provider_init(&t_turn_container, b, state) != 0)
   {
      b->release(b, state, 0);
      close(source_fd);
      return -1;
   }
   if (platform_random_hex(t_turn_container_epoch, 64))
   {
      b->release(b, state, 0);
      close(source_fd);
      return -1;
   }
   t_turn_container_source_fd = source_fd;
   snprintf(t_turn_container_source_cwd, sizeof t_turn_container_source_cwd, "%s", ws_real);
   workspace_provider_set_active(&t_turn_container.base);
   t_turn_container_backend = b;
   t_turn_container_state = state;
   t_turn_container_hibernate = bcfg.hibernate_on_exit;
   t_turn_bound = 1;
   LOG_INFO("delegate-sandbox",
            "delegate '%s': file/exec bound to its container (image=%s, workspace=%s, mode=%s)",
            task_id, bcfg.image ? bcfg.image : "<default>", bcfg.workspace,
            bcfg.workspace_read_only ? "ro" : "rw");
   return 1;
}

/* Test override: -1 = report the real bound state; 0/1 = force it. Lets guardrail/tool
 * tests exercise the container-delegate exemption without standing up a real backend. */
static int s_container_bound_test_override = -1;
void workspace_turn_set_container_bound_for_test(int bound)
{
   s_container_bound_test_override = bound;
}

int workspace_turn_container_bound(void)
{
   if (s_container_bound_test_override >= 0)
      return s_container_bound_test_override;
   return t_turn_container_backend != NULL && workspace_provider_active() == &t_turn_container.base;
}

void workspace_turn_unbind_active(void)
{
   if (t_turn_bound)
   {
      workspace_provider_clear_active(); /* no-op for the mirror path (none bound) */
      t_turn_bound = 0;
   }
   /* Release the container AFTER clearing the active pointer: it aliases
    * &t_turn_container.base, and an op racing a released container is worse than
    * one that finds no provider. Unconditional — a leaked container outlives the
    * turn and pins its workspace. */
   if (t_turn_container_backend && t_turn_container_state)
   {
      t_turn_container_backend->release(t_turn_container_backend, t_turn_container_state,
                                        t_turn_container_hibernate);
      t_turn_container_backend = NULL;
      t_turn_container_state = NULL;
      t_turn_container_hibernate = 0;
      t_turn_container_epoch[0] = 0;
   }
   if (t_turn_container_source_fd >= 0)
      close(t_turn_container_source_fd);
   t_turn_container_source_fd = -1;
   t_turn_container_source_cwd[0] = 0;
   t_turn_detached_epoch[0] = 0;
   if (t_turn_mirror_fd >= 0)
      close(t_turn_mirror_fd);
   t_turn_mirror_fd = -1;
   t_turn_cwd[0] = '\0';
   t_turn_drift[0] = '\0';
}

const char *workspace_turn_active_cwd(void)
{
   const char *cwd = t_nested_scope ? t_nested_scope->cwd : t_turn_cwd;
   return cwd[0] ? cwd : NULL;
}

const char *workspace_turn_drift_notice(void)
{
   const char *drift = t_nested_scope ? t_nested_scope->drift : t_turn_drift;
   return drift[0] ? drift : NULL;
}

int workspace_turn_reject_foreign_cwd(int detached_bound, int trusted_local, const char *cwd)
{
   if (detached_bound < 0)
      return 1;
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

/* Stable identity is measured by the resource owner; it grants no filesystem or
 * execution authority. Remote/container acquisition epochs cannot be forged by
 * an SDK or by a recycled provider address. */
int workspace_turn_resource_identity(const char *cwd, int *directory_fd, char digest[65])
{
   if (directory_fd)
      *directory_fd = -1;
   if (digest)
      digest[0] = 0;
   if (!cwd || !cwd[0] || !directory_fd || !digest)
      return -1;
   const workspace_provider_t *provider = workspace_provider_active();
   if (!provider)
      return -1;
   const char *epoch = "";
   int fd = -1;
   struct stat status = {0};
   if (provider->kind == WS_PROVIDER_CONTAINER)
   {
      if (provider != &t_turn_container.base || !t_turn_container_backend ||
          !t_turn_container_state || !t_turn_container_epoch[0])
         return -1;
      epoch = t_turn_container_epoch;
   }
   else if (provider->kind == WS_PROVIDER_DETACHED)
   {
      if (t_nested_scope && provider == &t_nested_scope->detached.base && t_nested_scope->bound)
         epoch = t_nested_scope->epoch;
      else if (provider == &t_turn_detached.base && t_turn_bound)
         epoch = t_turn_detached_epoch;
      if (!epoch[0])
         return -1;
   }
   else if (provider->kind == WS_PROVIDER_SHARED || provider->kind == WS_PROVIDER_MIRROR)
   {
      char actual[MAX_PATH_LEN];
      int mirror = -1;
      if (realpath(cwd, actual))
         mirror = active_mirror_lease(actual);
      if (mirror >= 0)
         fd = fcntl(mirror, F_DUPFD_CLOEXEC, 3);
      else if (project_owner && (fd = project_owner(cwd)) >= 0)
      {
         /* Retain the exact project-owner descriptor, never reopen by path. */
      }
      else if (workspace_turn_workspace_authorized(cwd, actual, sizeof actual))
         fd = open(actual, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0 || fstat(fd, &status))
      {
         if (fd >= 0)
            close(fd);
         return -1;
      }
   }
   else
      return -1;
   char identity[MAX_PATH_LEN + 256];
   int length = snprintf(identity, sizeof identity, "%d:%ju:%ju:%s:%s", provider->kind,
                         (uintmax_t)status.st_dev, (uintmax_t)status.st_ino, epoch, cwd);
   if (length < 0 || (size_t)length >= sizeof identity ||
       aimee_sha256_hex(identity, (size_t)length, digest))
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   *directory_fd = fd;
   return 0;
}

/* The mounted source is a host-owned capability, not the container execution
 * namespace. Callers may inspect this exact source for scoped verification and
 * project identity; no tool provider or execution grant changes here. */
int workspace_turn_source_identity(const char *cwd, int *directory_fd, char digest[65])
{
   if (directory_fd)
      *directory_fd = -1;
   if (digest)
      digest[0] = 0;
   if (!cwd || !cwd[0] || !directory_fd || !digest)
      return -1;
   const workspace_provider_t *provider = workspace_provider_active();
   if (!provider)
      return -1;
   if (provider->kind != WS_PROVIDER_CONTAINER)
   {
      if (workspace_turn_resource_identity(cwd, directory_fd, digest))
         return -1;
      if (*directory_fd >= 0)
         return 0;
      digest[0] = 0;
      return -1; /* A detached execution identity is not a local source. */
   }
   char actual[MAX_PATH_LEN];
   struct stat held, current;
   if (provider != &t_turn_container.base || !t_turn_container_backend || !t_turn_container_state ||
       t_turn_container_source_fd < 0 || !realpath(cwd, actual) ||
       strcmp(actual, t_turn_container_source_cwd) || fstat(t_turn_container_source_fd, &held) ||
       stat(actual, &current) || held.st_dev != current.st_dev || held.st_ino != current.st_ino ||
       !S_ISDIR(held.st_mode))
      return -1;
   int fd = fcntl(t_turn_container_source_fd, F_DUPFD_CLOEXEC, 3);
   if (fd < 0)
      return -1;
   char identity[MAX_PATH_LEN + 128];
   int length = snprintf(identity, sizeof identity, "workspace-source-v1:%ju:%ju:%s",
                         (uintmax_t)held.st_dev, (uintmax_t)held.st_ino, actual);
   if (length < 0 || (size_t)length >= sizeof identity ||
       aimee_sha256_hex(identity, (size_t)length, digest))
   {
      close(fd);
      digest[0] = 0;
      return -1;
   }
   *directory_fd = fd;
   return 0;
}
