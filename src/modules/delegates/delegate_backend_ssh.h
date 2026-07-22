/* delegate_backend_ssh.h: SSH execution backend.
 *
 * Long-term shape (per delegate-execution-backends.md):
 *   acquire   ssh + open ControlMaster socket with ControlPersist=300s,
 *             ensure ~/.aimee/delegate/<task_id>/ exists on the remote.
 *   exec      ssh -S <socket> bash -c '<wrapped>' so each call rides
 *             the same TCP connection (no per-call handshake cost).
 *   release   close the ControlMaster socket. hibernate=1 leaves the
 *             remote workspace dir; hibernate=0 rm -rf's it first.
 *
 * Helper functions below expose pure string-building and socket-wait
 * seams for tests; production code uses the registered backend vtable. */
#ifndef DEC_DELEGATE_BACKEND_SSH_H
#define DEC_DELEGATE_BACKEND_SSH_H 1

#include "delegate_backend.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Register the SSH backend in the process-global registry under the
    * name "ssh". Returns 0 on success, -1 on duplicate-name collision
    * (existing entry wins) or other registry failure. */
   int delegate_backend_register_ssh(void);

   /* Test seam: the static delegate_backend_ssh instance, for tests
    * that want to exercise the vtable directly without registry state.
    * Production code uses delegate_backend_lookup("ssh") after
    * register_ssh(). */
   delegate_backend_t *delegate_backend_ssh_get(void);

   /* Build a unique ControlMaster socket path for `task_id`. Prefers
    * $XDG_RUNTIME_DIR/aimee-ssh-<task_id>.sock (it's tmpfs and
    * cleaned per-session); falls back to /tmp/aimee-ssh-<uid>-<task_id>.sock
    * when XDG_RUNTIME_DIR is unset (the uid suffix avoids collisions
    * across users on shared boxes).
    *
    * Pure string assembly. Returns 0 on success, -1 on NULL/empty
    * task_id or undersized buffer. */
   int delegate_backend_ssh_socket_path(const char *task_id, char *out, size_t outsz);

   /* Poll until `path` exists as a regular file or socket, then
    * return 0. Returns -1 if `timeout_ms` elapses first. Polls every
    * 50ms — short enough that test setup feels instant, long enough
    * that we don't burn CPU.
    *
    * Used after spawning the SSH ControlMaster subprocess to confirm
    * the multiplex socket showed up before we hand its path to
    * subsequent ssh -S invocations. */
   int delegate_backend_ssh_wait_for_socket(const char *path, int timeout_ms);

   /* Build the ssh invocation command-string used by exec(). Pure
    * string assembly — no syscalls, no allocation beyond the result
    * buffer. The caller invokes the resulting string via execlp(bash,
    * "-c", out, ...) or similar.
    *
    * Shape of the produced command:
    *   ssh -S <ctrl_socket> -o ControlPath=<ctrl_socket>
    *       -o BatchMode=yes -o ServerAliveInterval=30
    *       <host> bash -c '<base64-encoded wrapped_script>'
    *
    * The wrapped_script (typically the output of
    * delegate_backend_wrap_command) is base64-encoded and decoded on
    * the remote so quoting is bullet-proof regardless of contents.
    *
    * Args:
    *   ctrl_socket   absolute path to the ControlMaster socket
    *   host          ssh target string (user@host or host)
    *   wrapped_script the bash script to run on the remote
    *   out_cmd       on success, *out_cmd is malloc'd (caller frees)
    *
    * Returns 0 on success; -1 on NULL inputs or allocation failure. */
   int delegate_backend_ssh_build_exec_command(const char *ctrl_socket, const char *host,
                                               const char *wrapped_script, char **out_cmd);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DELEGATE_BACKEND_SSH_H */
