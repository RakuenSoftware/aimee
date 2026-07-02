/* wfe_native_gate.h -- command-level externalization classifier for a primary
 * agent's NATIVE (non-aimee-MCP) tools. This is the follow-on to
 * wfe_externalization's name-only guard that its header calls out: externalization
 * performed THROUGH a general shell tool (a Bash tool running `git push` / a remote
 * `curl`) is invisible to name-matching, so an enforced run's pre-delivery guard
 * has a hole. This module inspects the shell command string to catch the common
 * cases. Pure policy: no DB / engine deps, unit-testable in isolation.
 *
 * ⚠️ HONEST SCOPE (consult overnight-design [1][25][28][35]): this REDUCES the
 * native externalization surface; it does NOT hermetically close it. A determined
 * agent can still egress via obfuscation this string match cannot see -- subshells
 * ($(), backticks, `bash -c`), encoded payloads (`base64 -d | sh`), heredocs, shell
 * functions/aliases, env-indirected binaries, or a tool this classifier does not
 * enumerate. Treat a NEGATIVE result as "no KNOWN externalization pattern", NEVER
 * as "provably safe". The full seal would require a sandbox (no network / read-only
 * mounts outside the worktree), tracked separately.
 */
#ifndef DEC_WFE_NATIVE_GATE_H
#define DEC_WFE_NATIVE_GATE_H 1

/* Bump when the pattern set changes so audit tooling can pin what was in effect. */
#define WFE_NATIVE_GATE_PATTERN_VERSION 1

/* 1 if `tool_name` is a general shell/command execution tool whose argument is a
 * command string we should inspect (Bash, bash, sh, shell, run_command, exec,
 * execute, terminal, ...). */
int wfe_is_shell_tool(const char *tool_name);

/* 1 if this native tool call is a KNOWN externalization (crosses the trust boundary
 * out of the worktree). Classification:
 *   - a named externalization primitive (delegates to wfe_is_externalization_tool);
 *   - a web/egress tool by name (WebFetch, WebSearch, fetch, ...);
 *   - a shell tool (wfe_is_shell_tool) whose `command` matches a known externalizing
 *     pattern: a git remote write (push / send-email), a `gh` mutation (pr / release
 *     / issue / api write), a remote transfer (scp / rsync-with-remote-spec / ssh
 *     remote-exec), a package publish (npm/cargo/twine/docker/pip upload), or a
 *     fetcher (curl/wget/nc/...) targeting an explicit NON-loopback URL/host.
 * `command` may be NULL/"" for non-shell tools (classified by name only).
 * Returns 0 when no known pattern matches -- NOT a safety proof (see header note). */
int wfe_native_tool_externalizes(const char *tool_name, const char *command);

#endif /* DEC_WFE_NATIVE_GATE_H */
