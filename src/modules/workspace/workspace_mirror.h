#ifndef WORKSPACE_MIRROR_H
#define WORKSPACE_MIRROR_H 1

#include <stddef.h>

/* workspace_mirror — the `detached` mirror tier (workspace-resource-plane §3).
 *
 * When aimee-server is truly remote (no bind-mount, no client filesystem), it
 * holds a server-side **bare mirror** of the workspace, seeded by `git fetch`
 * from the client-reported `vcs.remote` at the client-reported `head`. The
 * client's uncommitted diff is applied on top to reconstruct an equivalent
 * tree. This module owns the mirror lifecycle (seed / update) and — the part
 * AC #5 turns on — **drift detection**: client head vs server mirror head is
 * classified and surfaced, never silently merged.
 *
 * git invocation is injected (a function pointer) so the core is testable
 * without shelling out, and so production can route it through the workspace
 * provider / forge-credential exec env. */

/* How the client head relates to the server mirror head. */
typedef enum
{
   WS_MIRROR_IN_SYNC = 0,  /* identical heads */
   WS_MIRROR_CLIENT_AHEAD, /* mirror head is an ancestor of the client head */
   WS_MIRROR_MIRROR_AHEAD, /* client head is an ancestor of the mirror head */
   WS_MIRROR_DIVERGED,     /* neither is an ancestor of the other */
   WS_MIRROR_DRIFT_UNKNOWN /* a head was missing / ancestry could not be read */
} ws_mirror_drift_t;

/* Human-readable name for a drift verdict (stable; for surfacing to the user). */
const char *ws_mirror_drift_name(ws_mirror_drift_t d);

/* Pure classifier: given the two heads and the two ancestry facts
 * (a_is_ancestor_of_b: client head is an ancestor of the mirror head;
 *  b_is_ancestor_of_a: mirror head is an ancestor of the client head),
 * return the drift verdict. NULL/empty either head => DRIFT_UNKNOWN. No I/O. */
ws_mirror_drift_t ws_mirror_drift_classify(const char *client_head, const char *mirror_head,
                                           int client_is_ancestor_of_mirror,
                                           int mirror_is_ancestor_of_client);

/* git runner: execute `git <args...>` (NULL-terminated, not incl. "git"),
 * capturing trimmed stdout into out[out_cap] (may be NULL/0 when output is not
 * needed). Returns the command's exit status (0 = success), or <0 on spawn
 * failure. `ctx` is the opaque context passed to workspace_mirror_*; production
 * binds the forge-credential exec env, tests inject a mock. */
typedef int (*ws_git_runner_fn)(void *ctx, const char *const args[], char *out, size_t out_cap);

/* Ensure a bare mirror exists at `mirror_dir` seeded from `remote_url`:
 * clone --mirror when absent, else fetch. Returns 0 on success, -1 on failure.
 * git runs via `run`/`ctx`. */
int workspace_mirror_ensure(ws_git_runner_fn run, void *ctx, const char *remote_url,
                            const char *mirror_dir);

/* Read the mirror's commit id for `ref` (e.g. "HEAD", "refs/heads/main") into
 * out[out_cap]. Returns 0 on success, -1 if absent/unreadable. */
int workspace_mirror_head(ws_git_runner_fn run, void *ctx, const char *mirror_dir, const char *ref,
                          char *out, size_t out_cap);

/* Detect drift between `client_head` and the mirror's head for `ref`. Computes
 * the ancestry both ways via `git merge-base --is-ancestor` and classifies.
 * Returns the verdict (DRIFT_UNKNOWN if a head/ancestry can't be read). This
 * SURFACES drift; it never merges. */
ws_mirror_drift_t workspace_mirror_drift(ws_git_runner_fn run, void *ctx, const char *mirror_dir,
                                         const char *ref, const char *client_head);

/* Reconstruct an equivalent working tree of the client's state at `work_dir`
 * from the bare mirror at `mirror_dir`: materialize a detached worktree at
 * `head` (a resumed session re-points an existing worktree via a hard checkout
 * + clean), then apply the client's uncommitted unified diff found at
 * `diff_path` (NULL/empty = a clean checkout at head). Untracked files, absent
 * from a diff, are written separately by the caller via the workspace provider.
 *
 * The diff is the client's own working changes — not a secret — and the caller
 * has already written it to `diff_path` via the provider, so this TU stays pure
 * of filesystem I/O and runs git only through `run`/`ctx`. Returns 0 on success,
 * -1 if the checkout or `git apply` fails. */
int workspace_mirror_reconstruct(ws_git_runner_fn run, void *ctx, const char *mirror_dir,
                                 const char *work_dir, const char *head, const char *diff_path);

/* One-shot for a live detached turn: compute the drift (workspace_mirror_drift)
 * and format a user-facing summary line into out[out_cap] (e.g. "workspace
 * drift: client ahead of mirror — client a1b2c3d4, mirror f4e5d6c7; server
 * mirror is behind, fetch to sync"). Returns the verdict so the caller can both
 * surface `out` and gate on it (e.g. warn/refuse on DIVERGED). Safe with a NULL
 * `out`. */
ws_mirror_drift_t workspace_mirror_drift_report(ws_git_runner_fn run, void *ctx,
                                                const char *mirror_dir, const char *ref,
                                                const char *client_head, char *out, size_t out_cap);

/* Resolve the base directory the mirror tier stores its bare mirrors +
 * reconstructed worktrees under: AIMEE_WORKSPACES_DIR (an absolute path — e.g. a
 * persistent volume mounted into a detached/containerized server) when set, else
 * `<aimee_home>/workspaces`. Centralizes the base so every call site agrees and a
 * deployment can relocate the durable workspace state onto a mounted volume.
 * Returns 0 on success, -1 on env failure / truncation. */
int workspace_mirror_base(char *out, size_t cap);

/* Derive the deterministic server-side paths a `mirror` workspace's lifecycle
 * uses, under `base_dir` (e.g. "<aimee_home>/workspaces"): the bare mirror at
 * `<base_dir>/<hash(root)>/mirror` and the reconstructed worktree at
 * `<base_dir>/<hash(root)>/work`, where hash is an 8-hex FNV-1a of the client
 * workspace `root` (stable across processes; distinct roots → distinct dirs).
 * Pure string work — no I/O. Returns 0 on success, -1 on bad args / truncation. */
int workspace_mirror_paths(const char *base_dir, const char *root, char *mirror_dir, size_t md_cap,
                           char *work_dir, size_t wd_cap);

/* Path of the client's shipped uncommitted diff for `root`, under the same
 * hashed parent as workspace_mirror_paths: `<base_dir>/<hash(root)>/client.diff`.
 * The `workspace.mirror-sync` RPC writes the client's `git diff HEAD` here, and
 * the session setup feeds it to workspace_mirror_reconstruct as `diff_path` so
 * the reconstructed worktree mirrors the client's working tree. Pure string work;
 * returns 0 on success, -1 on bad args / truncation. */
int workspace_mirror_diff_path(const char *base_dir, const char *root, char *out, size_t out_cap);

/* Drive the mirror lifecycle for a detached session, from the registry's VCS
 * coordinates (workspace-resource-plane §3 — the wiring AC #5 turns on):
 *
 *   1. when `!already_materialized`, seed/update the bare mirror
 *      (workspace_mirror_ensure from `remote`);
 *   2. always compute + format the drift verdict (workspace_mirror_drift_report)
 *      of the client `head` vs the mirror, into `drift_out` (surfaced, never
 *      merged) and `*verdict_out`;
 *   3. when `!already_materialized`, reconstruct the worktree at `head` + the
 *      client diff (workspace_mirror_reconstruct).
 *
 * `already_materialized` is the caller's "the worktree already exists for this
 * session" check: a resumed session skips the (network) ensure + the
 * tree-destroying reconstruct so a multi-turn session's in-progress edits
 * survive; drift is still re-surfaced from the existing mirror. `diff_path` is
 * the client's uncommitted diff (NULL/"" = a clean checkout at head).
 *
 * git runs only via `run`/`ctx`; the only fs touch is the caller's own
 * `already_materialized` probe, so this stays mockable. Returns 0 on success,
 * -1 if ensure or reconstruct fails (the verdict is still reported on success).
 * `drift_out`/`verdict_out` may be NULL. */
int workspace_mirror_session_setup(ws_git_runner_fn run, void *ctx, const char *remote,
                                   const char *head, const char *mirror_dir, const char *work_dir,
                                   const char *diff_path, int already_materialized, char *drift_out,
                                   size_t drift_cap, ws_mirror_drift_t *verdict_out);

#endif /* WORKSPACE_MIRROR_H */
