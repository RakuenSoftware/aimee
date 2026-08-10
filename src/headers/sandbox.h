/*
 * sandbox.h: Linux namespace sandbox for tool execution.
 *
 * Wraps process spawning with OS-level isolation when enabled.
 * Falls back gracefully in environments where namespaces are unavailable
 * (nested containers, restricted kernels, Windows).
 *
 * Modes:
 *   SANDBOX_MODE_OFF            — no isolation; existing behaviour preserved
 *   SANDBOX_MODE_WORKSPACE_ONLY — visible/writable filesystem restricted to the
 *                                 session workspace plus runtime essentials
 *   SANDBOX_MODE_ALLOWLIST      — access restricted to configured paths plus
 *                                 runtime essentials
 *
 * Additional controls (per sandbox_config_t):
 *   network_isolated — block outbound network when supported
 */
#ifndef DEC_SANDBOX_H
#define DEC_SANDBOX_H 1

#include "platform.h"
#include <stddef.h>

/* Sandbox modes */
typedef enum
{
   SANDBOX_MODE_OFF = 0,            /* current behaviour — no isolation */
   SANDBOX_MODE_WORKSPACE_ONLY = 1, /* restrict FS to workspace + essentials */
   SANDBOX_MODE_ALLOWLIST = 2,      /* restrict FS to allow_paths + essentials */
} sandbox_mode_t;

#define SANDBOX_MAX_ALLOW_PATHS 16
#define SANDBOX_MAX_PATH_LEN    512

typedef struct
{
   sandbox_mode_t mode;
   int network_isolated; /* 1 = block outbound network (Linux only) */
   char allow_paths[SANDBOX_MAX_ALLOW_PATHS][SANDBOX_MAX_PATH_LEN];
   int allow_path_count;
} sandbox_config_t;

/*
 * sandbox_detect_container:
 *   Returns 1 if running inside a container (Docker, LXC, etc.) or a
 *   restricted environment where unshare/bind-mount would likely fail.
 *   Returns 0 otherwise.
 */
int sandbox_detect_container(void);

/*
 * sandbox_available:
 *   Returns 1 if the sandbox can be activated on this system (Linux, CAP_SYS_ADMIN
 *   or unprivileged user namespaces enabled, not already inside a container that
 *   prevents nesting).
 *   Returns 0 and sets *reason to a static diagnostic string on failure.
 */
int sandbox_available(const char **reason);

/*
 * sandbox_command_program:
 *   Extract the leading PROGRAM name of a /bin/sh -c command line into |out| — the
 *   first token after any `VAR=value` environment-assignment prefixes, basename
 *   only, never its arguments. This is a NON-SECRET label: a command line can
 *   carry secrets in its arguments or env assignments (tokens, keys), so only the
 *   program is ever surfaced (e.g. "TOKEN=sk-.. /usr/bin/npm install x" -> "npm").
 *   |out| is always NUL-terminated; empty for a NULL/blank command.
 */
void sandbox_command_program(const char *cmd, char *out, size_t out_len);

/*
 * Audit hook: notified when a guarded execution's isolation DEGRADES — the
 * sandbox was requested (mode != off) but was unavailable, so the command either
 * ran UNSANDBOXED (verdict "unsandboxed_fallback") or was REFUSED (verdict
 * "refused", for require-isolation callers). These rare, high-signal events are
 * the sandbox activity worth an audit trail; a routine successful sandboxed exec
 * is deliberately NOT recorded (high volume, low signal, would evict governance
 * rows). Only NON-SECRET fields cross: the program name (never arguments), the
 * mode, the network-isolation flag, the verdict, and the availability reason.
 *
 * Installed once at startup by a server-only bridge that forwards to the audit
 * event bus; sandbox.c itself has NO event-bus dependency (stays linkable into
 * every binary). NULL by default. Set once before serving.
 *
 * Coverage note (deliberate limitation): the hook fires for degradations the
 * PARENT observes — the sandbox being unavailable before the fork, and a
 * require-isolation child reporting failed in-namespace setup. A NON-require-
 * isolation exec whose child fails unshare()/mount-ns setup AFTER the fork execs
 * unsandboxed without a parent-visible signal, so that case is NOT audited. It is
 * rare (availability was already confirmed) and closing it would require the
 * parent to block on a child status byte on every exec; the trail therefore
 * records "requested-but-unavailable" and "refused" degradations, not every
 * possible post-fork downgrade.
 */
typedef void (*sandbox_audit_hook_fn)(const char *program, sandbox_mode_t mode,
                                      int network_isolated, const char *verdict,
                                      const char *reason);
void sandbox_set_audit_hook(sandbox_audit_hook_fn fn);

/*
 * Test-only seam: override the availability probe so the degraded-isolation audit
 * paths are deterministically reachable regardless of the host kernel/container.
 * Pass NULL to restore the real probe.
 */
void sandbox_set_available_override_for_test(int (*fn)(const char **reason));

/*
 * sandbox_effective_mode:
 *   The sandbox mode callers should GATE on (as distinct from the mode sandbox_exec
 *   builds with). Returns the test override when one is set, else cfg->mode; returns
 *   SANDBOX_MODE_OFF for a NULL cfg.
 *
 *   This exists because the delegate shell guard's fail-closed branch is only
 *   reachable when the mode is OFF, and now that the mode defaults to WORKSPACE_ONLY
 *   a test cannot reach it by configuration: the config path is resolved before a
 *   test can move HOME, so an in-process opt-out never reaches config_sandbox().
 *   Without this seam that containment branch would go unexercised.
 *
 * sandbox_set_mode_override_for_test:
 *   Force the value sandbox_effective_mode() reports. Pass -1 to clear. Test-only.
 */
int sandbox_effective_mode(const sandbox_config_t *cfg);
void sandbox_set_mode_override_for_test(int mode);

/*
 * sandbox_exec:
 *   Execute |cmd| via /bin/sh -c inside the sandbox described by |cfg|.
 *   The child's stdout/stderr are written to |out_fd|/|err_fd|.
 *   |workspace| is the path that workspace-only mode will expose.
 *
 *   Returns the child PID on success, -1 on error.
 *   If sandboxing is unavailable, logs a warning and falls back to plain fork/exec.
 *
 *   Only supported on POSIX (Linux + macOS stub). Windows always runs unsandboxed.
 */
#ifdef AIMEE_POSIX
pid_t sandbox_exec(const sandbox_config_t *cfg, const char *cmd, int out_fd, int err_fd,
                   const char *workspace);

/* Like sandbox_exec, but additionally bind-mounts read_only_path read-only and
 * then overlays write_path read-write when provided. Returns -1 instead of
 * falling back unsandboxed if namespace isolation is unavailable. */
pid_t sandbox_exec_with_readonly(const sandbox_config_t *cfg, const char *cmd, int out_fd,
                                 int err_fd, const char *workspace, const char *read_only_path,
                                 const char *write_path);
#endif /* AIMEE_POSIX */

/*
 * sandbox_mode_from_string:
 *   Parse a config string ("off", "workspace_only", "allowlist") into a
 *   sandbox_mode_t.  Returns SANDBOX_MODE_OFF on unknown input.
 */
static inline sandbox_mode_t sandbox_mode_from_string(const char *s)
{
   if (!s || s[0] == '\0' || (s[0] == 'o' && s[1] == 'f' && s[2] == 'f'))
      return SANDBOX_MODE_OFF;
   if (s[0] == 'w') /* workspace_only */
      return SANDBOX_MODE_WORKSPACE_ONLY;
   if (s[0] == 'a') /* allowlist */
      return SANDBOX_MODE_ALLOWLIST;
   return SANDBOX_MODE_OFF;
}

/*
 * sandbox_mode_to_string:
 *   Return a static string representation of |mode|.
 */
static inline const char *sandbox_mode_to_string(sandbox_mode_t mode)
{
   switch (mode)
   {
   case SANDBOX_MODE_OFF:
      return "off";
   case SANDBOX_MODE_WORKSPACE_ONLY:
      return "workspace_only";
   case SANDBOX_MODE_ALLOWLIST:
      return "allowlist";
   }
   return "off";
}

#endif /* DEC_SANDBOX_H */
