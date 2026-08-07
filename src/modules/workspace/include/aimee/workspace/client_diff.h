#ifndef AIMEE_WORKSPACE_CLIENT_DIFF_H
#define AIMEE_WORKSPACE_CLIENT_DIFF_H 1

/* The client's working-tree patch, as shipped to the server's mirror tier.
 *
 * The mirror reconstructs a server-side worktree by checking out the client's
 * head and applying this patch on top (workspace_mirror_reconstruct). Without
 * it the reconstruct is a clean checkout at head, so every uncommitted change
 * the client holds is silently absent from the tree an agent then works in.
 *
 * Shared because two callers need the same patch and it must be computed the
 * same way for both: `aimee workspace mirror-sync` (explicit) and the reverse
 * channel's registration (automatic, on attach). */

/* Compute the client's FULL working-tree patch vs HEAD for the repository at
 * `root` — tracked modifications, deletions, AND untracked (non-ignored) files
 * as additions — without touching the client's real index: stage everything into
 * a throwaway index (GIT_INDEX_FILE) seeded from HEAD, then
 * `diff --cached --binary HEAD`. git's --binary patch format is ASCII (base85
 * hunks), so the result is JSON-safe even for binary files.
 *
 * Returns a malloc'd patch the caller frees, which may be "" for a clean tree,
 * or NULL when `root` is not a repo / has no HEAD. POSIX-only (the thin Windows
 * client cannot fork git); Windows always returns NULL. */
char *workspace_client_diff_compute(const char *root);

#endif /* AIMEE_WORKSPACE_CLIENT_DIFF_H */
