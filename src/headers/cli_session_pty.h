/* cli_session_pty.h: server-hosted, PTY-attached CLI sessions.
 *
 * The server owns a tmux session running the CLI (e.g. `claude`) and a forkpty
 * child attached to it, so the raw terminal byte stream can be forwarded to a
 * thin client over /v1 (SSE output + queued input + resize) instead of the
 * lossy capture-pane screen-scrape. tmux gives the session persistence across
 * client disconnects; the PTY attach gives a faithful interactive terminal.
 *
 * Threading model: master_fd is touched only by the active stream pump
 * (cli_session_pty_stream). Input/resize callers enqueue state under a lock and
 * wake the pump via a self-pipe — they never write the PTY directly. The slot
 * is freed by whichever of kill()/stream() exits last ("last one out frees").
 */
#ifndef CLI_SESSION_PTY_H
#define CLI_SESSION_PTY_H

#include <stddef.h>

/* Feature gate (default off). Set at startup from config
 * (aimee.api.cli_session_forwarding); the /v1/cli/session* routes and the
 * streaming dispatch refuse with 404 when disabled. */
void cli_session_pty_set_forwarding(int on);
int cli_session_pty_forwarding_enabled(void);

/* Ensure a server-hosted PTY session exists for `id`. Creates a detached tmux
 * session running `cli_cmd` in `work_dir` (when absent) plus a forkpty child
 * attached to it; rows/cols seed the PTY window size. Idempotent: a live id is
 * a no-op success. Returns 0 on success, -1 on error (err filled if non-NULL). */
int cli_session_pty_ensure(const char *id, const char *cli_cmd, const char *work_dir, int rows,
                           int cols, char *err, size_t errn);

/* Queue raw input bytes for the session's PTY (typed into the attached
 * terminal) and wake the pump. Returns 0 on success, -1 on unknown id/overflow. */
int cli_session_pty_input(const char *id, const unsigned char *data, size_t len);

/* Request a window resize, applied by the pump via TIOCSWINSZ + SIGWINCH.
 * Returns 0 on success, -1 on unknown id. */
int cli_session_pty_resize(const char *id, int rows, int cols);

/* Stream raw PTY output to client_fd as SSE frames ("data: <base64>\n\n"),
 * draining queued input and applying resizes, until the client disconnects or
 * the CLI exits. Exactly one stream may attach at a time. The caller writes the
 * SSE response headers before calling. Returns 0 on clean end, -1 if the id is
 * unknown or already has an attached stream. */
int cli_session_pty_stream(const char *id, int client_fd);

/* Kill the PTY child + tmux session and free the slot. No-op for unknown id. */
void cli_session_pty_kill(const char *id);

/* Test seam: when set non-empty, ensure() execs this command on the PTY
 * directly (via `/bin/sh -c`) instead of `tmux attach`, and skips tmux session
 * creation — letting unit tests drive the pump with e.g. `cat`. Pass NULL/""
 * to restore production (tmux) behaviour. */
void cli_session_pty_set_attach_override(const char *cmd);

#endif /* CLI_SESSION_PTY_H */
