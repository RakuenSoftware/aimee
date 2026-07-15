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

/* Spawn a subprocess, write |input| to its stdin, capture stdout.
 * |cmd| is executed via /bin/sh -c (POSIX) or cmd.exe /c (Windows).
 * |input|/|input_len| is written to the child's stdin (may be NULL/0).
 * |out| receives the captured stdout (caller must free).
 * |out_len| receives the length.
 *
 * Returns the exit code, or -1 on spawn failure. */
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
