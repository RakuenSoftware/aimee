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
#define WFE_NATIVE_GATE_PATTERN_VERSION 3

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

/* 1 if this native tool call is FORBIDDEN OUTRIGHT — denied regardless of binding,
 * delivery, or enforce stage. This is orthogonal to the externalization truth table
 * below: externalization asks "does this cross the trust boundary, and is this
 * session allowed to yet?", whereas this asks "may an agent do this at all?".
 *
 * Currently: an admin override of branch protection (`gh pr merge --admin`, and the
 * `gh api` equivalent). Merging something that branch protection refuses is a
 * HUMAN-ONLY act (operator ruling 2026-07-15) — an agent may open a PR and merge a
 * green one, but may never force one past the rules. aimee's own merge paths no
 * longer offer a bypass; this closes the matching hole for an agent that types the
 * flag into a shell itself.
 *
 * Same honest-scope caveat as the header note: a NEGATIVE result means "no KNOWN
 * bypass pattern", never "provably safe". */
int wfe_native_tool_forbidden(const char *tool_name, const char *command);

/* 1 if this shell command INVOKES `git` or `gh` — as opposed to merely mentioning
 * one. `git push`, `/usr/bin/git push`, `sudo git push` and `bash -lc 'git push'`
 * match; `grep git file` and `echo git` do not (there it is an argument, not the
 * command). Command-prefix words (sudo/env/nohup/...) and VAR=val assignments are
 * looked through.
 *
 * Callers gate this on config `require_aimee_git` (default on) and deny: a delegate
 * runs no git/forge command itself; it uses aimee's git_* tools, which execute on
 * aimee-server where the forge credential lives in-process and never reaches a
 * child's environment or argv. Reads are included — git_status / git_log /
 * git_diff_summary / git_pr action=view cover them — so the rule needs no verb list
 * to keep current.
 *
 * The classifier is defence in depth, not the guarantee: it is a string match over a
 * shell line and the header's honest-scope note applies in full (subshells, base64,
 * env indirection). The guarantee is that delegates are spawned WITHOUT git/gh
 * credentials, so an evaded match still cannot reach the forge. */
int wfe_shell_invokes_git(const char *tool_name, const char *command);

typedef enum
{
   WFE_NATIVE_ALLOW = 0, /* not gated */
   WFE_NATIVE_WARN = 1,  /* warn-soak: log the would-deny, do not block */
   WFE_NATIVE_DENY = 2,  /* block (hard) */
} wfe_native_decision_t;

/* The S2 native-gate decision (pure truth table): an externalizing tool used by a
 * session BOUND to an enforced work-item that is NOT yet delivered (gate.deliver not
 * passed) is DENIED under a hard dial, WARNED otherwise. Everything else (not
 * externalizing / not bound / already delivered) is ALLOWED. `stage_hard` = the
 * binding's snapshotted enforce stage is hard. */
wfe_native_decision_t wfe_native_gate_decision(int externalizes, int bound, int delivered,
                                               int stage_hard);

#endif /* DEC_WFE_NATIVE_GATE_H */
