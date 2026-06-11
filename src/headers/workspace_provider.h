#ifndef WORKSPACE_PROVIDER_H
#define WORKSPACE_PROVIDER_H 1

#include <stddef.h>

/* Resource provider behind a workspace handle (workspace-resource-plane
 * proposal, Phase 1).
 *
 * The whole server assumes today that it runs as the user, on the user's host,
 * in the user's working tree: file tools open absolute paths directly. To let
 * aimee-server run detached (e.g. in a container) the proposal routes file/exec
 * access through a pluggable provider selected per workspace:
 *
 *   - `shared`   (co-located / bind-mount): direct filesystem on the server
 *                host. The default; preserves today's behavior with zero
 *                overhead. This is the only provider implemented in Phase 1.
 *   - `detached` (marshalled): the same primitives carried over /v1. Not yet
 *                implemented; resolving it falls back to `shared` for now.
 *
 * The provider is a NARROW boundary: it performs only raw, already-resolved
 * I/O. All policy — path resolution, `proposal:` handling, validation, the
 * parent-worktree write guard, line/offset formatting, diff/slop shaping —
 * stays ABOVE the provider in the caller, so it is identical for every
 * provider and `shared` compiles to "the same thing we do now." */

typedef enum
{
   WS_PROVIDER_SHARED = 0,   /* direct filesystem on the server host (default) */
   WS_PROVIDER_DETACHED = 1, /* marshalled over /v1 to the serving client */
   WS_PROVIDER_MIRROR = 2    /* server-side bare mirror + reconstructed worktree;
                              * file/exec run direct-fs on that local worktree
                              * (so it resolves through the `shared` provider),
                              * the lifecycle being driven from the registry's
                              * vcs.remote + head (workspace-resource-plane §3) */
} ws_provider_kind_t;

typedef struct
{
   int exists; /* 1 if the path exists */
   int is_dir; /* 1 if it is a directory */
   long size;  /* size in bytes (0 for directories / absent) */
} ws_stat_t;

typedef struct workspace_provider workspace_provider_t;

/* Streaming exec chunk callback: invoked with each piece of the child's stdout
 * as it is produced (binary-safe — `len` bytes at `data`, not NUL-terminated).
 * Return 0 to keep going, non-zero to abort the run early. */
typedef int (*ws_exec_chunk_fn)(void *cb_ctx, const char *data, size_t len);

struct workspace_provider
{
   ws_provider_kind_t kind;

   /* Read the entire file at `path` into a freshly malloc'd buffer with a
    * trailing NUL one past `*len` (binary-safe). On success returns 0 and sets
    * *out (caller frees) and *len; on open/read error returns -1 and leaves
    * *out NULL. */
   int (*read_all)(const workspace_provider_t *p, const char *path, char **out, size_t *len);

   /* Overwrite `path` with `len` bytes of `data` (truncating/creating).
    * data may be NULL when len is 0 (writes an empty file). Returns 0 on
    * success, -1 on open/write/close error. */
   int (*write_all)(const workspace_provider_t *p, const char *path, const char *data, size_t len);

   /* Fill `*st` for `path`. Always returns 0; st->exists is 0 when absent. */
   int (*stat)(const workspace_provider_t *p, const char *path, ws_stat_t *st);

   /* Glob `dir`/`pattern` (pattern NULL or "" means "*"). On success returns 0
    * and sets *out to a malloc'd array of *count malloc'd path strings (caller
    * frees each entry and the array); zero matches is success with *count 0.
    * Returns -1 on a glob error (not on no-match). */
   int (*list)(const workspace_provider_t *p, const char *dir, const char *pattern, char ***out,
               int *count);

   /* Run `argv` (NULL-terminated; argv[0] is the program, resolved on PATH),
    * capturing up to max_out bytes of combined stdout+stderr into *out (malloc'd,
    * NUL-terminated, caller frees; may be left NULL when there is no output).
    * Returns the command's exit status (0 = success), or non-zero on failure to
    * run. `shared` runs it on the server host; a future `detached` provider
    * dispatches it to the client-side runner. */
   int (*exec)(const workspace_provider_t *p, const char *const argv[], char **out, size_t max_out);

   /* Run a shell command-line `cmd` (as /bin/sh -c does), capturing combined
      stdout+stderr into a malloc'd NUL-terminated buffer (caller frees; may be
      NULL when there is no output) and returning it; sets *exit_code to the
      command's exit status (-1 on spawn failure). Unlike exec(argv,...) this
      honors shell features (redirections, pipes, quoted args) and the
      thread-local run_cmd working directory, so it is the seam for the legacy
      mcp_git shell-string call sites. `shared` runs it locally via run_cmd; a
      future `detached` provider marshals it to the client-side runner. */
   char *(*exec_shell)(const workspace_provider_t *p, const char *cmd, int *exit_code);

   /* Run `argv` (NULL-terminated) with `stdin_data` (`stdin_len` bytes, may be
    * NULL/0) written to the child's stdin, in working directory `cwd` (NULL =
    * inherit). The child's stdout is delivered incrementally via `on_chunk`
    * (may be NULL to discard); stderr is left on the child's stderr. Returns the
    * exit status (0 = success), or -1 on spawn failure. `shared` runs the
    * process on the server host; `detached` marshals it to the client-side
    * runner and streams the chunks back over the reverse channel. This is the
    * seam for running a local-CLI agent (e.g. `claude -p`) on the client. May be
    * NULL on a provider that does not support streaming exec. */
   int (*exec_stream)(const workspace_provider_t *p, const char *const argv[],
                      const char *stdin_data, size_t stdin_len, const char *cwd,
                      ws_exec_chunk_fn on_chunk, void *cb_ctx);
};

/* The process-wide `shared` (co-located) provider. Never NULL. */
const workspace_provider_t *workspace_provider_shared(void);

/* Resolve a provider by kind. Unimplemented kinds (currently everything but
 * WS_PROVIDER_SHARED) fall back to the shared provider. Never NULL. */
const workspace_provider_t *workspace_provider_for_kind(ws_provider_kind_t kind);

/* The provider the CURRENT THREAD's file/exec tools resolve through. Defaults
 * to the shared provider; a turn acting on a workspace whose provider is not
 * `shared` binds one for the turn's duration — mirroring the thread-local cwd
 * the same tools already use. Never NULL. set/clear bracket a turn. */
const workspace_provider_t *workspace_provider_active(void);
void workspace_provider_set_active(const workspace_provider_t *p);
void workspace_provider_clear_active(void);

/* Free a list() result (frees each entry, then the array). NULL-safe. */
void ws_provider_free_list(char **entries, int count);

/* Map a provider name to its kind / back. NULL or unknown name -> shared. */
ws_provider_kind_t ws_provider_kind_from_string(const char *name);
const char *ws_provider_kind_to_string(ws_provider_kind_t kind);

#endif /* WORKSPACE_PROVIDER_H */
