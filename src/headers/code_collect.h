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

/* Convenience wrapper over code_collect_files_cb that appends one
 * {"rel_path","content"} object per file to `files_arr`. Collects the whole
 * tree (no file-count cap); use when the caller pushes the result in one shot
 * and the tree is known-small. Returns the number of files appended. */
int code_collect_files(const char *root, cJSON *files_arr);

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
