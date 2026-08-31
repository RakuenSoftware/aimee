/* workspace_mirror.c — the `detached` mirror tier + drift detection.
 * See workspace_mirror.h and workspace-resource-plane §3. The git invocation is
 * injected (ws_git_runner_fn) so this TU is pure of process/credential plumbing
 * and unit-tests with a mock; production binds the forge-credential exec env. */
#include "workspace_mirror.h"
#include "aimee_home.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int workspace_mirror_base(char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   /* A deployment (e.g. a detached/containerized server) points the mirror tier
    * at a PERSISTENT mounted volume via AIMEE_WORKSPACES_DIR so the bare mirrors
    * + reconstructed worktrees survive restarts; otherwise the state lives under
    * the instance config home. */
   const char *env = getenv("AIMEE_WORKSPACES_DIR");
   int n;
   if (env && env[0] == '/')
      n = snprintf(out, cap, "%s", env);
   else
   {
      const char *home = aimee_home();
      if (!home || !home[0])
         return -1;
      n = snprintf(out, cap, "%s/workspaces", home);
   }
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

const char *ws_mirror_drift_name(ws_mirror_drift_t d)
{
   switch (d)
   {
   case WS_MIRROR_IN_SYNC:
      return "in_sync";
   case WS_MIRROR_CLIENT_AHEAD:
      return "client_ahead";
   case WS_MIRROR_MIRROR_AHEAD:
      return "mirror_ahead";
   case WS_MIRROR_DIVERGED:
      return "diverged";
   default:
      return "unknown";
   }
}

ws_mirror_drift_t ws_mirror_drift_classify(const char *client_head, const char *mirror_head,
                                           int client_is_ancestor_of_mirror,
                                           int mirror_is_ancestor_of_client)
{
   if (!client_head || !client_head[0] || !mirror_head || !mirror_head[0])
      return WS_MIRROR_DRIFT_UNKNOWN;
   if (strcmp(client_head, mirror_head) == 0)
      return WS_MIRROR_IN_SYNC;
   /* mirror is behind the client (mirror head is an ancestor of client head) */
   if (mirror_is_ancestor_of_client && !client_is_ancestor_of_mirror)
      return WS_MIRROR_CLIENT_AHEAD;
   /* client is behind the mirror (client head is an ancestor of mirror head) */
   if (client_is_ancestor_of_mirror && !mirror_is_ancestor_of_client)
      return WS_MIRROR_MIRROR_AHEAD;
   /* neither is an ancestor of the other → genuine divergence */
   return WS_MIRROR_DIVERGED;
}

static int dir_exists(const char *path)
{
   struct stat st;
   return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int workspace_mirror_ensure(ws_git_runner_fn run, void *ctx, const char *remote_url,
                            const char *mirror_dir)
{
   if (!run || !remote_url || !remote_url[0] || !mirror_dir || !mirror_dir[0])
      return -1;

   /* A seeded bare mirror has an objects/ dir; treat its presence as "exists". */
   char objects[1100];
   snprintf(objects, sizeof(objects), "%s/objects", mirror_dir);
   if (dir_exists(objects))
   {
      const char *fetch[] = {"-C", mirror_dir, "fetch", "--prune", "origin", NULL};
      return run(ctx, fetch, NULL, 0) == 0 ? 0 : -1;
   }
   const char *clone[] = {"clone", "--mirror", remote_url, mirror_dir, NULL};
   return run(ctx, clone, NULL, 0) == 0 ? 0 : -1;
}

int workspace_mirror_head(ws_git_runner_fn run, void *ctx, const char *mirror_dir, const char *ref,
                          char *out, size_t out_cap)
{
   if (out && out_cap)
      out[0] = '\0';
   if (!run || !mirror_dir || !mirror_dir[0] || !ref || !ref[0] || !out || out_cap == 0)
      return -1;
   const char *args[] = {"-C", mirror_dir, "rev-parse", ref, NULL};
   if (run(ctx, args, out, out_cap) != 0 || !out[0])
   {
      out[0] = '\0';
      return -1;
   }
   /* trim trailing whitespace/newline the runner may leave */
   size_t n = strlen(out);
   while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
      out[--n] = '\0';
   return out[0] ? 0 : -1;
}

/* 1 iff `ancestor` is an ancestor of `descendant` in the mirror (incl. equal),
 * via `git merge-base --is-ancestor` (exit 0 = yes, 1 = no). */
static int is_ancestor(ws_git_runner_fn run, void *ctx, const char *mirror_dir,
                       const char *ancestor, const char *descendant)
{
   const char *args[] = {"-C",     mirror_dir, "merge-base", "--is-ancestor",
                         ancestor, descendant, NULL};
   return run(ctx, args, NULL, 0) == 0 ? 1 : 0;
}

ws_mirror_drift_t workspace_mirror_drift(ws_git_runner_fn run, void *ctx, const char *mirror_dir,
                                         const char *ref, const char *client_head)
{
   if (!run || !client_head || !client_head[0])
      return WS_MIRROR_DRIFT_UNKNOWN;

   char mirror_head[128];
   if (workspace_mirror_head(run, ctx, mirror_dir, ref ? ref : "HEAD", mirror_head,
                             sizeof(mirror_head)) != 0)
      return WS_MIRROR_DRIFT_UNKNOWN;

   if (strcmp(client_head, mirror_head) == 0)
      return WS_MIRROR_IN_SYNC;

   /* Ancestry is meaningful only if both commits exist in the mirror. The
    * client head may not be present (the client is genuinely ahead with commits
    * the mirror never fetched); merge-base returns non-zero then, which we read
    * as "not an ancestor". */
   int client_anc_mirror = is_ancestor(run, ctx, mirror_dir, client_head, mirror_head);
   int mirror_anc_client = is_ancestor(run, ctx, mirror_dir, mirror_head, client_head);
   return ws_mirror_drift_classify(client_head, mirror_head, client_anc_mirror, mirror_anc_client);
}

int workspace_mirror_reconstruct(ws_git_runner_fn run, void *ctx, const char *mirror_dir,
                                 const char *work_dir, const char *head, const char *diff_path)
{
   if (!run || !mirror_dir || !mirror_dir[0] || !work_dir || !work_dir[0] || !head || !head[0])
      return -1;

   /* Materialize a working tree at `head`. A fresh session adds a detached
    * worktree off the bare mirror; `worktree add` fails when work_dir is already
    * populated (a resumed session), so fall back to a hard checkout + clean to
    * reach the same state idempotently. */
   const char *add[] = {"-C",      mirror_dir, "worktree", "add", "--detach",
                        "--force", work_dir,   head,       NULL};
   if (run(ctx, add, NULL, 0) != 0)
   {
      const char *co[] = {"-C", work_dir, "checkout", "--detach", "--force", head, NULL};
      if (run(ctx, co, NULL, 0) != 0)
         return -1;
      const char *clean[] = {"-C", work_dir, "clean", "-fd", NULL};
      run(ctx, clean, NULL, 0); /* best-effort: drop a prior turn's stray untracked */
   }

   /* Apply the client's working-tree patch on top, if any. A clean client still
    * publishes a zero-byte snapshot file, so --allow-empty makes that valid on
    * the first Git call instead of leaving a correctly-created but rejected
    * worktree behind. `--binary` covers binary hunks; the same patch also carries
    * tracked mods, deletions, and untracked-file additions. */
   if (diff_path && diff_path[0])
   {
      const char *apply[] = {"-C",       work_dir,  "apply", "--allow-empty", "--whitespace=nowarn",
                             "--binary", diff_path, NULL};
      if (run(ctx, apply, NULL, 0) != 0)
         return -1;
   }
   return 0;
}

int workspace_mirror_reconstruct_branch(ws_git_runner_fn run, void *ctx, const char *remote,
                                        const char *mirror_dir, const char *work_dir,
                                        const char *head, const char *branch, const char *upstream,
                                        const char *diff_path)
{
   if (!run || !remote || !remote[0] || !mirror_dir || !mirror_dir[0] || !work_dir ||
       !work_dir[0] || !head || !head[0] || !branch || !branch[0])
      return -1;

   /* A normal clone has its own refs/index while sharing immutable objects with
    * the durable mirror.  Multiple client snapshot generations may therefore
    * retain the same logical branch name without Git's shared-worktree branch
    * exclusion or cross-generation ref updates. */
   const char *clone[] = {"clone", "--no-checkout", "--shared", mirror_dir, work_dir, NULL};
   if (run(ctx, clone, NULL, 0) != 0)
      return -1;
   const char *set_url[] = {"-C", work_dir, "remote", "set-url", "origin", remote, NULL};
   if (run(ctx, set_url, NULL, 0) != 0)
      return -1;
   const char *checkout[] = {"-C", work_dir, "checkout", "-B", branch, head, NULL};
   if (run(ctx, checkout, NULL, 0) != 0)
      return -1;
   if (upstream && upstream[0])
   {
      const char *track[] = {"-C", work_dir, "branch", "--set-upstream-to", upstream, branch, NULL};
      if (run(ctx, track, NULL, 0) != 0)
         return -1;
   }
   if (diff_path && diff_path[0])
   {
      const char *apply[] = {"-C",       work_dir,  "apply", "--allow-empty", "--whitespace=nowarn",
                             "--binary", diff_path, NULL};
      if (run(ctx, apply, NULL, 0) != 0)
         return -1;
   }
   return 0;
}

ws_mirror_drift_t workspace_mirror_drift_report(ws_git_runner_fn run, void *ctx,
                                                const char *mirror_dir, const char *ref,
                                                const char *client_head, char *out, size_t out_cap)
{
   if (out && out_cap)
      out[0] = '\0';
   ws_mirror_drift_t d = workspace_mirror_drift(run, ctx, mirror_dir, ref, client_head);

   char mhead[128];
   mhead[0] = '\0';
   workspace_mirror_head(run, ctx, mirror_dir, ref ? ref : "HEAD", mhead, sizeof(mhead));
   const char *ch = client_head ? client_head : "";

   if (out && out_cap)
   {
      switch (d)
      {
      case WS_MIRROR_IN_SYNC:
         snprintf(out, out_cap, "workspace in sync (head %.10s)", ch);
         break;
      case WS_MIRROR_CLIENT_AHEAD:
         snprintf(out, out_cap,
                  "workspace drift: client ahead of mirror — client %.10s, mirror %.10s; "
                  "server mirror is behind, fetch to sync",
                  ch, mhead);
         break;
      case WS_MIRROR_MIRROR_AHEAD:
         snprintf(out, out_cap,
                  "workspace drift: mirror ahead of client — client %.10s, mirror %.10s; "
                  "the client checkout is behind the remote",
                  ch, mhead);
         break;
      case WS_MIRROR_DIVERGED:
         snprintf(out, out_cap,
                  "workspace drift: DIVERGED — client %.10s and mirror %.10s share no ancestor; "
                  "reconcile before writing",
                  ch, mhead);
         break;
      default:
         snprintf(out, out_cap, "workspace drift: unknown — a head could not be read");
         break;
      }
   }
   return d;
}

/* 8-hex FNV-1a of `s` into out[9]. Stable across processes (no salt). */
static void fnv1a_hex8(const char *s, char out[9])
{
   uint32_t h = 2166136261u;
   for (const unsigned char *p = (const unsigned char *)s; p && *p; p++)
   {
      h ^= *p;
      h *= 16777619u;
   }
   snprintf(out, 9, "%08x", h);
}

int workspace_mirror_paths(const char *base_dir, const char *root, char *mirror_dir, size_t md_cap,
                           char *work_dir, size_t wd_cap)
{
   if (!base_dir || !base_dir[0] || !root || !root[0] || !mirror_dir || !work_dir)
      return -1;
   char hash[9];
   fnv1a_hex8(root, hash);
   int m = snprintf(mirror_dir, md_cap, "%s/%s/mirror", base_dir, hash);
   int w = snprintf(work_dir, wd_cap, "%s/%s/work", base_dir, hash);
   if (m < 0 || (size_t)m >= md_cap || w < 0 || (size_t)w >= wd_cap)
      return -1;
   return 0;
}

int workspace_mirror_diff_path(const char *base_dir, const char *root, char *out, size_t out_cap)
{
   if (!base_dir || !base_dir[0] || !root || !root[0] || !out)
      return -1;
   char hash[9];
   fnv1a_hex8(root, hash);
   int n = snprintf(out, out_cap, "%s/%s/client.diff", base_dir, hash);
   if (n < 0 || (size_t)n >= out_cap)
      return -1;
   return 0;
}

static int snapshot_path(const char *base_dir, const char *root, const char *kind,
                         unsigned long long generation, const char *digest, char *out,
                         size_t out_cap)
{
   if (!base_dir || !base_dir[0] || !root || !root[0] || !kind || !kind[0] || !out)
      return -1;
   char hash[9];
   fnv1a_hex8(root, hash);
   int n = generation ? snprintf(out, out_cap, "%s/%s/%s-%llu-%s", base_dir, hash, kind, generation,
                                 digest ? digest : "")
                      : snprintf(out, out_cap, "%s/%s/%s", base_dir, hash, kind);
   if (n < 0 || (size_t)n >= out_cap)
      return -1;
   return 0;
}

int workspace_mirror_snapshot_meta_path(const char *base_dir, const char *root, char *out,
                                        size_t out_cap)
{
   return snapshot_path(base_dir, root, "client.snapshot", 0, NULL, out, out_cap);
}

int workspace_mirror_snapshot_diff_path(const char *base_dir, const char *root,
                                        unsigned long long generation, const char *digest,
                                        char *out, size_t out_cap)
{
   if (!generation || !digest || strlen(digest) != 64)
      return -1;
   return snapshot_path(base_dir, root, "client.diff", generation, digest, out, out_cap);
}

int workspace_mirror_snapshot_work_path(const char *base_dir, const char *root,
                                        unsigned long long generation, const char *digest,
                                        char *out, size_t out_cap)
{
   if (!generation || !digest || strlen(digest) != 64)
      return -1;
   return snapshot_path(base_dir, root, "work", generation, digest, out, out_cap);
}

int workspace_mirror_oid_valid(const char *oid)
{
   if (!oid)
      return 0;
   size_t len = strlen(oid);
   if (len != 40 && len != 64)
      return 0;
   for (const unsigned char *p = (const unsigned char *)oid; *p; p++)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

int workspace_mirror_ref_valid(const char *ref)
{
   if (!ref || !ref[0])
      return 1;
   size_t len = strlen(ref);
   if (ref[0] == '-' || ref[0] == '/' || ref[len - 1] == '/' || ref[len - 1] == '.' ||
       strcmp(ref, "@") == 0 || strstr(ref, "..") || strstr(ref, "@{") || strstr(ref, "//"))
      return 0;

   const char *component = ref;
   for (const unsigned char *p = (const unsigned char *)ref; *p; p++)
   {
      if (*p <= 0x20 || *p == 0x7f || *p == '\\' || *p == '~' || *p == '^' || *p == ':' ||
          *p == '?' || *p == '*' || *p == '[')
         return 0;
      if ((const char *)p == component && *p == '.')
         return 0;
      if (*p == '/')
      {
         size_t component_len = (const char *)p - component;
         if (component_len >= 5 && memcmp(p - 5, ".lock", 5) == 0)
            return 0;
         component = (const char *)p + 1;
      }
   }
   size_t component_len = ref + len - component;
   return !(component_len >= 5 && memcmp(ref + len - 5, ".lock", 5) == 0);
}

int workspace_mirror_snapshot_parse(const char *text, ws_mirror_snapshot_t *out)
{
   if (!text || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   char branch[256] = "-", upstream[256] = "-", extra = '\0';
   int fields = sscanf(text, "%llu %64[a-f0-9] %64[a-f0-9] %255s %255s %llu %c", &out->generation,
                       out->digest, out->head, branch, upstream, &out->sync_order, &extra);
   if ((fields != 3 && fields != 5 && fields != 6) || !out->generation ||
       strlen(out->digest) != 64 || !workspace_mirror_oid_valid(out->head) ||
       (fields == 6 && !out->sync_order))
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }
   if (fields >= 5)
   {
      if (strcmp(branch, "-") != 0)
         snprintf(out->branch, sizeof(out->branch), "%s", branch);
      if (strcmp(upstream, "-") != 0)
         snprintf(out->upstream, sizeof(out->upstream), "%s", upstream);
   }
   return 0;
}

static int transfer_id_valid(const char *transfer)
{
   if (!transfer || strlen(transfer) != 32)
      return 0;
   for (const unsigned char *p = (const unsigned char *)transfer; *p; p++)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

int workspace_mirror_transfer_paths(const char *base_dir, const char *root, const char *transfer,
                                    char *assembly, size_t assembly_cap, char *state,
                                    size_t state_cap, char *lock, size_t lock_cap, char *counter,
                                    size_t counter_cap)
{
   if (!base_dir || !base_dir[0] || !root || !root[0] || !transfer_id_valid(transfer) ||
       !assembly || !state || !lock || !counter)
      return -1;
   char hash[9];
   fnv1a_hex8(root, hash);
   int a = snprintf(assembly, assembly_cap, "%s/%s/client.diff.%s", base_dir, hash, transfer);
   int s = snprintf(state, state_cap, "%s/%s/client.sync.%s", base_dir, hash, transfer);
   int l = snprintf(lock, lock_cap, "%s/%s/client.sync.lock", base_dir, hash);
   int c = snprintf(counter, counter_cap, "%s/%s/client.sync.counter", base_dir, hash);
   return a < 0 || (size_t)a >= assembly_cap || s < 0 || (size_t)s >= state_cap || l < 0 ||
                  (size_t)l >= lock_cap || c < 0 || (size_t)c >= counter_cap
              ? -1
              : 0;
}

int workspace_mirror_transfer_state_parse(const char *text, ws_mirror_transfer_state_t *out)
{
   if (!text || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   char extra = '\0';
   int fields = sscanf(text, "%llu %u %c", &out->order, &out->next_seq, &extra);
   if (fields != 2 || !out->order || !out->next_seq)
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }
   return 0;
}

int workspace_mirror_snapshot_next_generation(const ws_mirror_snapshot_t *current, int have_current,
                                              unsigned long long order, const char *digest,
                                              unsigned long long *generation_out)
{
   if (!order || !digest || strlen(digest) != 64 || !generation_out || (have_current && !current))
      return -1;
   if (have_current && current->sync_order > order)
      return -1; /* a later-started transfer has already published */
   if (have_current && strcmp(current->digest, digest) == 0)
      *generation_out = current->generation;
   else
   {
      if (have_current && current->generation == ULLONG_MAX)
         return -1;
      *generation_out = have_current ? current->generation + 1 : 1;
   }
   return 0;
}

int workspace_mirror_session_setup(ws_git_runner_fn run, void *ctx, const char *remote,
                                   const char *head, const char *mirror_dir, const char *work_dir,
                                   const char *diff_path, int already_materialized, char *drift_out,
                                   size_t drift_cap, ws_mirror_drift_t *verdict_out)
{
   if (drift_out && drift_cap)
      drift_out[0] = '\0';
   if (verdict_out)
      *verdict_out = WS_MIRROR_DRIFT_UNKNOWN;
   if (!run || !mirror_dir || !mirror_dir[0] || !work_dir || !work_dir[0] || !head || !head[0])
      return -1;

   /* A fresh session seeds the bare mirror; a resumed one reuses the existing
    * mirror (skipping the network fetch) so a multi-turn edit-in-progress tree
    * is not blown away by the reconstruct below. */
   if (!already_materialized && (!remote || !remote[0]))
      return -1;
   if (!already_materialized && workspace_mirror_ensure(run, ctx, remote, mirror_dir) != 0)
      return -1;

   /* Surface drift (client head vs the mirror's HEAD) — never merge. Cheap
    * rev-parse + ancestry on the existing mirror, safe to run every turn. */
   ws_mirror_drift_t d =
       workspace_mirror_drift_report(run, ctx, mirror_dir, "HEAD", head, drift_out, drift_cap);
   if (verdict_out)
      *verdict_out = d;

   /* Reconstruct the equivalent worktree only on first materialization — the
    * checkout/clean would otherwise discard a resumed session's working edits. */
   if (!already_materialized &&
       workspace_mirror_reconstruct(run, ctx, mirror_dir, work_dir, head, diff_path) != 0)
      return -1;

   return 0;
}

int workspace_mirror_session_setup_branch(ws_git_runner_fn run, void *ctx, const char *remote,
                                          const char *head, const char *branch,
                                          const char *upstream, const char *mirror_dir,
                                          const char *work_dir, const char *diff_path,
                                          int already_materialized, char *drift_out,
                                          size_t drift_cap, ws_mirror_drift_t *verdict_out)
{
   if (!branch || !branch[0])
      return workspace_mirror_session_setup(run, ctx, remote, head, mirror_dir, work_dir, diff_path,
                                            already_materialized, drift_out, drift_cap,
                                            verdict_out);
   if (drift_out && drift_cap)
      drift_out[0] = '\0';
   if (verdict_out)
      *verdict_out = WS_MIRROR_DRIFT_UNKNOWN;
   if (!run || !mirror_dir || !mirror_dir[0] || !work_dir || !work_dir[0] || !head || !head[0])
      return -1;
   if (!already_materialized && (!remote || !remote[0]))
      return -1;
   if (!already_materialized && workspace_mirror_ensure(run, ctx, remote, mirror_dir) != 0)
      return -1;
   ws_mirror_drift_t d =
       workspace_mirror_drift_report(run, ctx, mirror_dir, "HEAD", head, drift_out, drift_cap);
   if (verdict_out)
      *verdict_out = d;
   if (!already_materialized &&
       workspace_mirror_reconstruct_branch(run, ctx, remote, mirror_dir, work_dir, head, branch,
                                           upstream, diff_path) != 0)
      return -1;
   return 0;
}
