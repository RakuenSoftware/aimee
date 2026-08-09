#ifndef AIMEE_WORKSPACE_CLIENT_DIFF_H
#define AIMEE_WORKSPACE_CLIENT_DIFF_H 1

#include <stddef.h>

/* The client's working-tree patch, as shipped to the server's mirror tier.
 *
 * The mirror reconstructs a server-side worktree by checking out a commit it can
 * FETCH and applying this patch on top (workspace_mirror_reconstruct). Both
 * halves have to agree on the same base, which is why resolving the base and
 * computing the patch live together here.
 *
 * Shared because two callers need the identical pair: `aimee workspace
 * mirror-sync` (explicit) and the reverse channel's registration (automatic, on
 * attach). A second implementation that picked a different base would produce a
 * patch the server cannot apply. */

/* Resolve the mirror BASE for the repository at `root`: the newest ancestor of
 * HEAD that exists on some remote, i.e. the newest commit the server can
 * actually `git fetch`.
 *
 * HEAD itself is the wrong answer whenever the developer has local commits that
 * were never pushed — the server cannot fetch them, so the reconstruct fails and
 * the delegate gets nothing. Resolving to the last pushed ancestor instead lets
 * those commits ride along inside the patch as ordinary working-tree content,
 * which is what a delegate needs to build against. (Their commit HISTORY is not
 * reproduced server-side; the file contents are.)
 *
 * Returns 0 and writes a 40-hex commit id into out[out_cap] on success. Returns
 * -1 when no ancestor of HEAD exists on any remote — an unpushed-only repository
 * that no server could reconstruct — and the caller must refuse rather than
 * guess. POSIX-only; the thin Windows client cannot fork git and always gets -1. */
int workspace_client_mirror_base(const char *root, char *out, size_t out_cap);

/* Compute the client's FULL working-tree patch against `base` for the repository
 * at `root` — tracked modifications, deletions, AND untracked (non-ignored)
 * files as additions — without touching the client's real index: stage
 * everything into a throwaway index (GIT_INDEX_FILE) seeded from `base`, then
 * `diff --cached --binary <base>`. git's --binary patch format is ASCII (base85
 * hunks), so the result is JSON-safe even for binary files.
 *
 * `base` must be what workspace_client_mirror_base returned for the same root,
 * and must be the same commit registered as the workspace's head — the patch is
 * meaningless against any other commit.
 *
 * Returns a malloc'd patch the caller frees, which may be "" for a tree that
 * matches `base` exactly, or NULL when `root` is not a repo or `base` is
 * unreadable. POSIX-only; Windows always returns NULL. */
char *workspace_client_diff_compute(const char *root, const char *base);

/* The decision inside workspace_client_mirror_base, separated from running git
 * so it can be tested directly: given the output of
 * `git rev-list --boundary HEAD --not --remotes` and the resolved HEAD id,
 * choose the base.
 *
 *   no output            -> nothing unpushed, base = `head`
 *   a "-<sha>" line      -> base = that boundary commit (the last pushed one)
 *   output, no "-" line  -> unpushed with no fetchable ancestor -> -1
 *
 * Returns 0 on success, -1 when the caller must refuse. */
int workspace_client_mirror_base_select(const char *revlist_out, const char *head, char *out,
                                        size_t out_cap);

#endif /* AIMEE_WORKSPACE_CLIENT_DIFF_H */
