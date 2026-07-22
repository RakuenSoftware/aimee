#ifndef GIT_PROJECT_H
#define GIT_PROJECT_H 1

#include <stddef.h>

/* git_project — clone a remote repo as a project under a webchat user's scoped
 * workspace (webchat-git WP-D). The destination is resolved + validated by
 * workspace_scope (no cross-tenant escape), the user's vaulted git credentials
 * are injected (WP-C) into the git child env only, and `git clone` runs without
 * a shell (argv + envp), so a hostile URL cannot inject a command or a flag.
 * Indexing the cloned project is the caller's concern (index_scan_project). */

/* Clone `url` as an org-scoped project under `principal` (a `webuser:<name>`).
 * The repo name derives from `name` (when non-empty) or the URL basename minus
 * a trailing ".git"; the org derives from `org` (when non-empty, sanitized) or
 * the URL's single-segment owner path (multi-segment owners bail to a flat
 * clone). The resulting ref ("<org>/<repo>" or flat "<repo>") is registered in
 * the deployment-global key registry under the first-component lifecycle lock
 * BEFORE the clone publishes (temp dir + atomic rename, destination pinned by
 * openat2 fds); any pre-publish failure rolls the registration back. `token`
 * is an optional access token: when non-NULL/non-empty it authenticates the
 * clone AND is persisted as the host's credential (git_host_cred). On success
 * writes the absolute project path to out_path[path_cap] and the project REF
 * to out_ref[ref_cap], and returns 0. On failure returns -1 with a short,
 * non-sensitive message in err[errlen] (canonical remotes in messages are
 * credential-free; a registry conflict never echoes the other remote), or
 * GP_ERR_CONFLICT (-2) when the failure is an identity conflict (existing
 * project, flat/org namespace clash, same-key-different-remote) so callers
 * can 409 instead of 400. Requires openat2 (Linux >= 5.6); fails closed
 * otherwise. */
#define GP_ERR_CONFLICT (-2)
int git_project_clone(const char *principal, const char *url, const char *name, const char *org,
                      const char *token, char *out_path, size_t path_cap, char *out_ref,
                      size_t ref_cap, char *err, size_t errlen);

/* The server-local lexical index delete seam used by git_project_delete:
 * DELETED ROW COUNT (>= 0) on success, -1 on failure. Weak — a 0-success
 * no-op in the split AIMEE_DB2_DISABLED server (no local index there); the
 * combined build deletes the local db2 rows; tests override it. */
int gp_local_index_delete(const char *ref);

/* Normalize `url` to the credential-free canonical remote (scheme://host/path,
 * userinfo/query/fragment/".git" stripped) into out[cap]. 0 or -1. */
int git_project_canonical_remote(const char *url, char *out, size_t cap);

/* Derive the sanitized org component from `url`'s owner path into out[cap].
 * Returns 0 on success; -1 when no single-segment owner is derivable (sets
 * *multi_segment when the owner path had multiple segments — GitLab
 * subgroups — which bail to flat by design). */
int git_project_derive_org(const char *url, char *out, size_t cap, int *multi_segment);

/* For a multi-segment owner URL: the sanitized candidate org segments, comma
 * joined ("group, sub"), for the org_note guidance. 0 or -1. */
int git_project_org_candidates(const char *url, char *out, size_t cap);

/* --- delete (webchat project lifecycle proposal, slice 2) ---------------- */

/* Outcome of one git_project_delete operation. `kb_detail` (when non-NULL) is
 * a malloc'd JSON string — the kb purge's per-store detail on success/force,
 * or {"error": …} on an abort — suitable for embedding as the response's
 * "kb" member. The caller frees it. */
typedef struct
{
   char kb_status[16];  /* "purged" | "retained" | "forced" */
   char purge_id[40];   /* random hex id shared by every audit line + the fence */
   char generation[40]; /* monotonic fence generation (last-holder purges only) */
   char *kb_detail;     /* malloc'd JSON or NULL; caller frees */
} git_project_delete_result_t;

/* The ref did not resolve under the caller's tree (callers 404 — plain "not
 * found", no cross-principal existence disclosure). */
#define GP_ERR_NOT_FOUND (-3)
/* The kb purge failed or was unreachable and force was not set: nothing
 * filesystem was destroyed; callers 503 with res->kb_detail. */
#define GP_ERR_KB_UNAVAILABLE (-4)

/* Delete `principal`'s project `ref` (proposal slice 2, steps 1-8). Under the
 * first-component lifecycle lock: audit intent, resolve strictly under the
 * caller (else GP_ERR_NOT_FOUND), decide holders via the registry
 * (resync-then-unregister); the LAST holder runs the fenced kb purge (abort
 * with GP_ERR_KB_UNAVAILABLE and a rolled-back decrement unless `force`),
 * then the server-local lexical index delete, the fd-relative filesystem
 * removal, org-dir prune, and purge-finalize. Every phase logs one
 * webuser_project_delete_audit_v1 line sharing res->purge_id. Returns 0,
 * GP_ERR_NOT_FOUND, GP_ERR_KB_UNAVAILABLE, or -1 (validation/internal) with a
 * short message in err[errlen]. */
int git_project_delete(const char *principal, const char *ref, int force,
                       git_project_delete_result_t *res, char *err, size_t errlen);

/* Read the credential-free canonical remote of `principal`'s project `ref`
 * (the .aimee/remote sidecar) into out[cap]. 0 or -1 (legacy clones without a
 * sidecar return -1; callers fall back to git config or show nothing). */
int git_project_remote(const char *principal, const char *ref, char *out, size_t cap);

/* List `principal`'s project REFS ("<org>/<repo>" or flat "<repo>") into
 * out[max][GIT_PROJECT_NAME_MAX]; two-level walk with the structural rule
 * (first-level dir with .git = flat project, without = org dir). Returns the
 * count (>=0), or -1 on a bad principal. A missing scope root is 0 projects,
 * not an error. */
#define GIT_PROJECT_NAME_MAX 131 /* WS_REF_MAX (129) + NUL, padded */
int git_project_list(const char *principal, char out[][GIT_PROJECT_NAME_MAX], int max);

#endif /* GIT_PROJECT_H */
