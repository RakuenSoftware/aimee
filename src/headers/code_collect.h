/* code_collect.h: shared, DB-free source-file collector.
 *
 * Walks a directory tree and gathers indexable, non-binary source files as
 * {"rel_path","content"} cJSON objects — the wire shape the aimee-kb
 * /v1/code/scan handler accepts for push-based indexing. Pure filesystem +
 * cJSON, with no kb_client / DB dependency, so it links into BOTH the engine
 * (server-side local scans) and the thin client (which pushes its own tree to
 * a remote server that cannot see the client filesystem). POSIX only; a no-op
 * returning 0 elsewhere.
 */
#ifndef CODE_COLLECT_H
#define CODE_COLLECT_H

#include "cJSON.h"

/* Per-file size bound: skip anything larger so a single file can't blow up a
 * batch. There is intentionally NO cap on the number of files collected — the
 * tree is bounded by the directory skips and per-file filters, and callers that
 * must keep a single network request small stream the files into byte-sized
 * batches (code_collect_files_cb) rather than truncating at a file count. */
#define CODE_COLLECT_MAX_FILE_BYTES (256 * 1024)

/* Streaming collector: recursively walk `root` and invoke `cb` once per
 * indexable, non-binary, non-oversized file with its project-relative path and
 * NUL-terminated content (both owned by the walker — copy what you keep). Skips
 * VCS/build/hidden directories. Best-effort: per-file errors are silently
 * skipped. `cb` returns 0 to continue or non-zero to stop the walk early.
 * Returns the number of files for which `cb` was invoked and returned 0. This
 * is the no-limit path: it lets a caller flush batches as it walks so an
 * arbitrarily large tree never has to sit in memory at once. */
typedef int (*code_collect_file_cb)(const char *rel_path, const char *content, void *ctx);
int code_collect_files_cb(const char *root, code_collect_file_cb cb, void *ctx);

/* Immutable identity of the committed Git snapshot read by the collector.
 * ref is the requested/resolved branch or ref; commit_sha identifies history,
 * tree_sha identifies the exact path->blob manifest. is_default is true only
 * when the caller omitted a ref and default-ref resolution selected it. */
typedef struct
{
   char ref[1024];
   char commit_sha[128];
   char tree_sha[128];
   int is_default;
} code_source_snapshot_t;

/* Resolve and read one explicit committed branch/ref without checking it out.
 * requested_ref=NULL/empty selects the repository default. The working tree is
 * never used as a fallback for an explicit ref. snapshot_out may be NULL.
 * Returns the number of collected files, or 0 when the ref is unavailable. */
int code_collect_files_at_ref_cb(const char *root, const char *requested_ref,
                                 code_collect_file_cb cb, void *ctx,
                                 code_source_snapshot_t *snapshot_out);

/* Resolve a branch/ref to immutable commit+tree identities without collecting
 * files. requested_ref=NULL/empty selects the default branch. */
int code_resolve_source_snapshot(const char *root, const char *requested_ref,
                                 code_source_snapshot_t *out);

/* Resolve the branch actually checked out at `root`. Detached HEAD fails
 * closed: callers that need detached-commit operation must opt into that exact
 * commit explicitly rather than silently treating the repository default as
 * the active source context. */
int code_resolve_current_snapshot(const char *root, code_source_snapshot_t *out);

/* Determine whether an explicit ref's tip is already reachable from the
 * repository default tip. Returns 1 merged, 0 not merged, -1 unresolved/error.
 * The default ref itself is never classified as merged. */
int code_source_ref_is_merged(const char *root, const char *source_ref);

/* Determine whether `source_ref` is the branch currently checked out at `root`.
 * Returns 1 when it is, 0 otherwise (including detached HEAD, which has no
 * attached branch). Retirement uses this to leave live work alone: a branch
 * created off the default and a branch fast-forward-merged into it are
 * indistinguishable from refs alone, so the checkout is what tells them apart. */
int code_source_ref_is_current_checkout(const char *root, const char *source_ref);

/* Convenience wrapper over code_collect_files_cb that appends one
 * {"rel_path","content"} object per file to `files_arr`. Collects the whole
 * tree (no file-count cap); use when the caller pushes the result in one shot
 * and the tree is known-small. Returns the number of files appended. */
int code_collect_files(const char *root, cJSON *files_arr);

/* Compatibility default-branch tree resolver. New generation-aware callers
 * should retain the full code_source_snapshot_t above. */
int git_resolve_default_sha(const char *root, char *out, size_t outlen);
int code_default_branch_changed(const char *stored_sha, const char *current_sha);
/* 1 if AIMEE_CODE_INDEX_SOURCE=worktree (the index tracks WIP, so the default-branch
 * SHA gate must NOT be applied). */
int code_index_source_is_worktree(void);

/* §6 live: install a post-merge git hook in `project_root` that backgrounds
 * `aimee index scan <project_name> <project_root>` so the code graph re-indexes after a
 * merge/pull advances the default branch. 0 on success, -1 on error, -2 if a non-aimee
 * post-merge hook already exists (left untouched). */
int code_index_install_branch_hook(const char *project_root, const char *project_name);

/* Discover git repositories under `root` for one-project-per-repo ingest: invoke
 * `cb` once per real checkout (a directory whose `.git` is itself a directory),
 * with the repo's absolute path. Linked worktrees (`.git` is a regular file) are
 * skipped — they are duplicate working copies of an already-tracked repo — and
 * symlinks are never followed (cycle guard). A worktree passed as `root` itself
 * is honored. Kept here (not workspace.c) so the thin client can use it without
 * the heavier workspace.o dependency chain. POSIX only; no-op elsewhere. Returns
 * the number of repos for which `cb` was invoked. */
typedef void (*code_collect_repo_cb)(const char *repo_abs, void *ctx);
int code_collect_discover_repos(const char *root, code_collect_repo_cb cb, void *ctx);

#endif /* CODE_COLLECT_H */
