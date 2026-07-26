/*
 * platform_process.h: portable process spawning.
 *
 * Wraps fork+exec (POSIX) / CreateProcess (Windows) for the two
 * main patterns used in aimee:
 *   1. Daemon spawn (detach from parent, redirect stdio)
 *   2. Capture spawn (run command, capture stdout/stderr)
 */
#ifndef DEC_PLATFORM_PROCESS_H
#define DEC_PLATFORM_PROCESS_H 1

#include "platform.h"
#include "platform_random.h"
#include <sys/types.h>

typedef int (*platform_process_cancel_fn_t)(void *ctx);

/* Spawn a daemon process. The child:
 *   - calls setsid() (POSIX) or detaches (Windows)
 *   - redirects stdin/stdout/stderr to /dev/null (or NUL)
 *   - execs |argv[0]| with the given argument list (NULL-terminated)
 *
 * Returns the child PID on success (to the parent), -1 on error.
 * The parent does not wait for the child. */
pid_t platform_spawn_daemon(const char *const argv[]);

/* Spawn a subprocess and capture its stdout.
 * |cmd| is executed via /bin/sh -c (POSIX) or cmd.exe /c (Windows).
 * |out| receives the captured stdout (caller must free).
 * |out_len| receives the length.
 * |timeout_ms| is the max execution time (0 = no limit).
 *
 * Returns the exit code, or -1 on spawn failure. */
int platform_exec_capture(const char *cmd, char **out, size_t *out_len, int timeout_ms);

/* Same as platform_exec_capture, but polls cancel_fn while the child is
 * running. If cancellation is requested, terminates the child process and
 * returns -1 with any captured output collected so far. */
int platform_exec_capture_cancellable(const char *cmd, char **out, size_t *out_len, int timeout_ms,
                                      platform_process_cancel_fn_t cancel_fn, void *cancel_ctx);

/* Distinct failure reasons. Negative so they cannot collide with an exit code.
 * A caller that only checks `rc != 0` still behaves correctly; one that wants to
 * distinguish "the command failed" from "we gave up on it" can. */
#define PLATFORM_EXEC_ERR_SPAWN        (-1)
#define PLATFORM_EXEC_ERR_TIMEOUT      (-2)
#define PLATFORM_EXEC_ERR_OUTPUT_LIMIT (-3)

/* Defaults for platform_exec_pipe(). Deliberately generous: they exist so that
 * NO call site is unbounded, not to be the right answer for any particular one.
 * A caller with a known shape should use platform_exec_pipe_bounded. */
#define PLATFORM_EXEC_DEFAULT_TIMEOUT_MS 120000
#define PLATFORM_EXEC_DEFAULT_MAX_OUTPUT ((size_t)(64 * 1024 * 1024))

/* Spawn a subprocess, write |input| to its stdin, capture stdout, BOUNDED.
 *
 * |cmd| runs via /bin/sh -c (POSIX) or cmd.exe /c (Windows).
 * |input|/|input_len| goes to the child's stdin (may be NULL/0).
 * |out| receives captured stdout (caller frees); |out_len| its length.
 * |timeout_ms| bounds the WHOLE exchange — write, read and wait share one
 *   monotonic deadline, so the total is bounded rather than each step separately.
 * |max_output| caps captured bytes; exceeding it terminates the child.
 *
 * Both pipes are serviced concurrently under poll(). This is not an optimisation:
 * writing all input before reading any output deadlocks whenever both directions
 * exceed pipe capacity, which was a live defect.
 *
 * The child is placed in its own process group; on timeout or limit the GROUP is
 * terminated (SIGTERM, grace, then SIGKILL) so grandchildren cannot survive
 * holding the pipe. The child is always reaped — no zombies on any path.
 *
 * Returns the child's exit code (>= 0), or one of the PLATFORM_EXEC_ERR_*
 * constants. |out| is set only on a non-negative return. */
int platform_exec_pipe_bounded(const char *cmd, const char *input, size_t input_len, char **out,
                               size_t *out_len, int timeout_ms, size_t max_output);

/* Convenience wrapper over platform_exec_pipe_bounded with the defaults above.
 * Present so that existing call sites are bounded rather than unbounded; prefer
 * the explicit form when the command's shape is known. */
int platform_exec_pipe(const char *cmd, const char *input, size_t input_len, char **out,
                       size_t *out_len);

/* Get the full path of the current executable.
 * Writes up to |size| bytes into |buf| (including NUL terminator).
 * Returns 0 on success, -1 on failure. */
int platform_get_exe_path(char *buf, size_t size);

/* Portable setenv.
 * POSIX: setenv(name, value, 1). Windows: SetEnvironmentVariable(). */
int platform_setenv(const char *name, const char *value);

/* Portable getuid.
 * POSIX: getuid(). Windows: returns 0 (no direct equivalent). */
unsigned int platform_getuid(void);

/* Handler function type: takes the signal number (POSIX) or
 * a Windows control event code mapped to a pseudo-signal number. */
typedef void (*platform_signal_handler_t)(int signum);

/* Install a handler for SIGTERM-equivalent (graceful shutdown).
 * POSIX: signal(SIGTERM, handler). Windows: SetConsoleCtrlHandler. */
void platform_signal_term(platform_signal_handler_t handler);

/* Install a handler for SIGINT-equivalent (Ctrl+C).
 * POSIX: signal(SIGINT, handler). Windows: SetConsoleCtrlHandler. */
void platform_signal_int(platform_signal_handler_t handler);

/* Send SIGTERM-equivalent to a process by PID.
 * POSIX: kill(pid, SIGTERM). Windows: OpenProcess + TerminateProcess. */
int platform_signal_send_term(int pid);

#endif /* DEC_PLATFORM_PROCESS_H */
