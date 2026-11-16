/* delegate_backend.h: pluggable delegate execution backend interface.
 *
 * Today aimee shells delegate work out through one of two ad-hoc paths:
 * a local posix_spawn pool, and an SSH command runner. The
 * delegate-execution-backends proposal generalises both into a single
 * `delegate_backend_t` vtable with three implementations — local, ssh,
 * docker — picked per task by `agents.json` or a CLI flag.
 *
 * This header defines the interface and a tiny in-process registry.
 * The server registers the shipped local, ssh, and docker backends at
 * startup.
 *
 * Lifecycle in plain English:
 *   acquire(task_id) -> opaque state handle
 *   exec(state, cmd) -> result; may be called many times
 *   read_file/write_file/list_dir/get_cwd/set_cwd act on the workspace
 *   release(state, hibernate) -> tears down; hibernate=1 means keep
 *      workspace materials so the next acquire(same task_id) resumes.
 *
 * The registry is process-global, populated once at server start, and
 * never mutated again — lookups are lock-free reads. */
#ifndef DEC_DELEGATE_BACKEND_H
#define DEC_DELEGATE_BACKEND_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Maximum number of backends that may register in one process.
    * The shipped set is local + ssh + docker (3); the
    * cap leaves room for plugin-supplied backends without dynamic
    * allocation. */
#define DELEGATE_BACKEND_MAX 16

   /* acquire() return code for a HARD isolation refusal. The backend tore the
    * container down and the delegate MUST NOT run or degrade to a host path. */
#define DELEGATE_ACQUIRE_REFUSED_ISOLATION (-2)

   /* Per-acquire configuration that the caller passes through. The
    * fields are advisory — backends silently ignore values they do not
    * use (e.g., `image` is meaningless to the local backend). */
   typedef struct delegate_backend_config
   {
      const char *image;     /* docker image hint, NULL for default */
      const char *host;      /* ssh target host, NULL for local */
      int hibernate_on_exit; /* if 1, release() preserves workspace state */
      /* Host directory to expose AS the workspace. Required: an empty scratch
       * checkout is not a source tree and is refused. Pointing this at the tree
       * the delegate already has server-side
       * (its worktree, or the session cwd) gives the container the ENTIRE CURRENT
       * SOURCE TREE by bind-mount — no copy, no sync, no drift: it IS the tree.
       *
       * The backend does NOT create this path; a caller naming a directory that
       * does not exist is asking to mount something it has not made. */
      const char *workspace;
      /* Mount `workspace` READ-ONLY. Required whenever the tree is not the
       * delegate's own: a delegate's changes must stay inside its container, so a
       * tree it shares with anything else must be unwritable at the MOUNT — not
       * merely guarded above it. A guard is a rule; a read-only bind is a property.
       */
      int workspace_read_only;
      /* 1 when delegate_sandbox_package_access == "proxy": the docker backend arms the
       * in-container package forwarder + http_proxy env. The caller resolves the mode
       * (it already loads config) so the backend stays config-agnostic. */
      int pkg_proxy;
      /* Deprecated compatibility field. Enabled sandboxes always verify and
       * fail closed; no caller value can weaken that invariant. */
      int require_isolation;
   } delegate_backend_config_t;

   /* Result of an `exec` call. `stdout_buf` and `stderr_buf` are
    * caller-allocated; backends write into them (NUL-terminated, may be
    * truncated to fit). `exit_code` is the wrapped command's exit code,
    * not the transport's — a transport failure surfaces via the int
    * return value of `exec`. */
   typedef struct delegate_exec_result
   {
      int exit_code;
      int latency_ms;
      char *stdout_buf;
      size_t stdout_cap;
      char *stderr_buf;
      size_t stderr_cap;
   } delegate_exec_result_t;

   /* The backend vtable. All function pointers must be set; a backend
    * that does not support a given op returns -1 with a sensible
    * errno-equivalent in its implementation rather than leaving the
    * slot NULL. */
   typedef struct delegate_backend
   {
      const char *name;        /* canonical key, e.g. "local", "docker" */
      const char *description; /* one-line human-readable */

      int (*acquire)(struct delegate_backend *self, const char *task_id,
                     const delegate_backend_config_t *cfg, void **state_out);
      void (*release)(struct delegate_backend *self, void *state, int hibernate);

      int (*exec)(struct delegate_backend *self, void *state, const char *command, int timeout_ms,
                  delegate_exec_result_t *result_out);

      int (*read_file)(struct delegate_backend *self, void *state, const char *path, int offset,
                       int limit, char **out);
      int (*write_file)(struct delegate_backend *self, void *state, const char *path,
                        const char *content);
      int (*list_dir)(struct delegate_backend *self, void *state, const char *path,
                      char ***entries_out);

      int (*get_cwd)(struct delegate_backend *self, void *state, char **out);
      int (*set_cwd)(struct delegate_backend *self, void *state, const char *path);
   } delegate_backend_t;

   /* Add a backend to the process-global registry. Returns 0 on success,
    * -1 on duplicate name (the existing entry wins; callers can detect
    * and skip), -1 if the registry is full, -1 if `b` or `b->name` is
    * NULL/empty. The pointer is stored as-is — backends are typically
    * static globals, so no copy is taken. */
   int delegate_backend_register(delegate_backend_t *b);

   /* Look up a backend by name. Returns NULL if no match. The returned
    * pointer is owned by the registry; callers must not free it. */
   delegate_backend_t *delegate_backend_lookup(const char *name);

   /* Snapshot of the registry. Writes up to `max` pointers into `out`
    * and returns the count actually written. The order matches
    * registration order. Useful for `aimee delegate backends` and tests. */
   int delegate_backend_list(delegate_backend_t **out, int max);

   /* Render the registry as a JSON array suitable for an RPC response
    * or operator inspection:
    *   [{"name":"local","description":"..."},
    *    {"name":"ssh","description":"..."}]
    * Returns a malloc'd NUL-terminated string (caller frees) on success
    * and NULL on allocation failure. An empty registry returns "[]".
    * Order matches registration order. */
   char *delegate_backend_list_json(void);

   /* Test seam: empty the registry. Tests call this between cases so
    * static backend registrations from one test do not leak into the
    * next. Production code never calls this. */
   void delegate_backend_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DELEGATE_BACKEND_H */
