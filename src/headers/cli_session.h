#ifndef CLI_SESSION_H
#define CLI_SESSION_H

#include <time.h>
#include <stddef.h>

#define CLI_SESSION_NAME_MAX 128
#define CLI_SESSION_CMD_MAX  256
#define CLI_SESSION_BUF_MAX  (256 * 1024)
#define CLI_SESSION_POLL_MS  500 /* poll interval for completion detection */
#define CLI_SESSION_STABLE_N 3   /* consecutive stable polls to declare done */
/* When the CLI is stuck in a provider error/retry state (claude renders this as
 * its ✻ status line: "API error · Retrying in Ns · attempt K/10"), the pane
 * animates the retry counter forever, so the stability heuristic never fires and
 * only the full idle timeout (often minutes) would break it — a silent "Working"
 * spinner with no answer. recv bounds that error state separately: after this
 * many ms continuously in a provider-error state with no answer produced, it
 * stops the retry and returns -4 with a clear error. claude retries ~10× with
 * short backoff (well under this default), so a transient blip it recovers from
 * leaves the error state — and the turn completes normally — before the grace
 * elapses; the bound only bites on a sustained provider failure.
 * Opt-in: only applied when the caller sets it via cli_session_set_error_grace_ms
 * (default 0 = disabled, preserving the legacy timeout-only behaviour). */
#define CLI_SESSION_DEFAULT_ERROR_GRACE_MS 60000

typedef struct
{
   char session_name[CLI_SESSION_NAME_MAX]; /* tmux session name */
   char cli_cmd[CLI_SESSION_CMD_MAX];       /* CLI command that was started */
   char cli_kind[32];                       /* "claude"/"codex"/... drives response parsing */
   int reuse;                               /* 1 = reuse across tasks */
   time_t last_activity;
   int active; /* 1 = session exists */
   /* Pane snapshot taken just before a prompt is sent. recv diffs against it so
    * a reused pane returns ONLY this turn's output, never prior turns still on
    * screen. malloc'd; freed by cli_session_destroy. */
   char *baseline;
   /* Clean response text already streamed to the caller this turn (so recv emits
    * only the growth as an incremental delta). malloc'd; freed on destroy. */
   char *stream_emitted;
   /* Per-session claude HOME minted by cli_session_isolated_claude_home for an
    * isolated delegate seat (NULL when the seat runs in the shared HOME). Owned by
    * the session: cli_session_destroy rm-rf's the tree and frees this, so the home
    * is reclaimed the moment the delegate exits instead of lingering for the 1h
    * age-based sweep. malloc'd. */
   char *iso_home;
} cli_session_t;

/* Incremental stream callback: invoked by cli_session_recv with each newly
 * produced chunk of clean response text as the turn streams. Set per-thread. */
typedef void (*cli_session_stream_cb_t)(const char *delta, void *ud);
void cli_session_set_stream_cb(cli_session_stream_cb_t cb, void *ud);
/* Read the current thread's stream callback so a caller can save/restore it
 * around a nested turn (prevents the inner turn's now-dead context leaking to
 * the outer turn). *ud_out receives the userdata; returns the callback. */
cli_session_stream_cb_t cli_session_get_stream_cb(void **ud_out);

/* Cancel-check callback: cli_session_recv polls it each tick; a non-zero return
 * aborts the wait (the running turn was asked to stop — steering/interrupt or
 * session close). recv then sends an interrupt key to the pane so the CLI stops
 * generating (the conversation stays intact for the next turn) and returns the
 * cancelled status. Thread-local, like the stream callback. */
typedef int (*cli_session_cancel_cb_t)(void *ud);
void cli_session_set_cancel_check(cli_session_cancel_cb_t cb, void *ud);
cli_session_cancel_cb_t cli_session_get_cancel_check(void **ud_out);

/* Liveness heartbeat callback: cli_session_recv invokes it periodically (throttled
 * to ~CLI_SESSION_HEARTBEAT_MS) while it waits for the CLI to finish a turn. A
 * tmux-CLI delegate (e.g. claude) runs the model loop inside the CLI process, so
 * aimee's HTTP progress heartbeat never fires and the stale-idle monitor would
 * cancel a long-but-healthy turn. This proves the recv loop is alive; a genuinely
 * wedged CLI is still bounded by recv's own idle timeout. Thread-local, saved and
 * restored around a nested turn like the stream/cancel callbacks. */
typedef void (*cli_session_heartbeat_cb_t)(void *ud);
void cli_session_set_heartbeat_cb(cli_session_heartbeat_cb_t cb, void *ud);
cli_session_heartbeat_cb_t cli_session_get_heartbeat_cb(void **ud_out);
#define CLI_SESSION_HEARTBEAT_MS 15000

/* Provider-error grace (thread-local, ms): how long recv tolerates the CLI
 * sitting in a provider error/retry state before giving up with -4. 0 disables
 * the bound (legacy: only the idle timeout applies). Save/restore around a nested
 * turn like the stream/cancel callbacks. See CLI_SESSION_DEFAULT_ERROR_GRACE_MS. */
void cli_session_set_error_grace_ms(int ms);
int cli_session_get_error_grace_ms(void);

/* Record the CLI kind so recv can pick the right TUI response parser. */
void cli_session_set_kind(cli_session_t *s, const char *cli_kind);

/* Snapshot the current pane as the baseline for the next recv. Call right
 * before cli_session_send so the turn's reply is diffed from a clean boundary. */
void cli_session_mark_baseline(cli_session_t *s);

/* Resolve the directory a SERVER-HOSTED CLI session should run in. Writes the
 * chosen dir into `out` (turn_cwd when it exists on this host, else the server
 * process cwd) and returns 1 iff it FELL BACK — i.e. a non-empty turn_cwd was
 * absent here and had to be replaced. On fallback the caller must retarget
 * run_cmd_set_cwd() to `out`, else run_cmd's `cd '<turn_cwd>' &&` prefix fails
 * every session shell-out ("failed to create tmux session"). Returns 0 when
 * turn_cwd is usable or empty (nothing to retarget). */
int cli_session_resolve_cwd(const char *turn_cwd, char *out, size_t n);

/* --- Lifecycle --- */
/* Creates (or attaches to existing) tmux session, starts CLI.
 * session_name must be unique (e.g. "aimee-<agent>-<taskid>").
 * Returns 0 on success, -1 on failure. */
int cli_session_create(cli_session_t *s, const char *session_name, const char *cli_cmd,
                       const char *work_dir, int reuse);

/* Pre-seed claude-code's config so its interactive TUI starts straight at the
 * prompt instead of blocking on first-run gates the headless `claude -p` path
 * skipped: onboarding, the --dangerously-skip-permissions warning, and the
 * per-workspace "trust this folder?" dialog. aimee runs each session in its own
 * (fresh) worktree, so the trust dialog re-appears every session and wedges the
 * pane; this trusts `work_dir`. Best-effort and idempotent — never fails the
 * turn (a worst case just re-shows a prompt). Call before creating a claude
 * session. No-op for non-claude providers (caller gates on cli_kind).
 *
 * `autonomous`: when nonzero the launch passes --dangerously-skip-permissions,
 * so also pre-accept its one-time warning (skipDangerousModePermissionPrompt).
 * When zero, only onboarding + per-folder trust are seeded (those are required
 * for the TUI to start at all); the bypass warning is left untouched. */
void cli_session_prepare_claude(const char *work_dir, int autonomous);

/* Mint a per-session claude HOME so concurrent delegate seats don't share one
 * ~/.claude.json + ~/.claude runtime tree (which claude-code writes non-atomically
 * all turn, wedging concurrent seats). Only the OAuth token + settings.json are
 * shared (symlinked); the rest is per-turn. Best-effort: returns 0 and fills
 * home_out on success, -1 on any failure (caller then uses the shared HOME).
 * Reaps per-session homes older than an hour on each call. */
int cli_session_isolated_claude_home(const char *work_dir, char *home_out, size_t home_out_sz);

/* Refresh the shared codex OAuth token file (<home>/.codex/auth.json) from the
 * vault before a codex delegate launches, so the vault stays the single source of
 * truth. Best-effort: 0 on success or when no codex token is vaulted (legacy file
 * left as-is), -1 only on a write failure. */
int cli_session_materialize_codex_oauth(void);

/* Materialize seam: read the vaulted (agent, "oauth") token into `out`. The strong
 * implementation (server_cli_oauth.c) reads the vault; a weak default in
 * cli_session.c returns non-zero so binaries without the vault fall back to the
 * legacy on-disk credential. Returns 0 (token written) or non-zero (no entry). */
int cli_oauth_vault_materialize_get(const char *agent, char *out, size_t out_len);

/* Hand a minted isolated HOME (from cli_session_isolated_claude_home) to the
 * session so cli_session_destroy reclaims it on exit. Copies `home`; pass NULL to
 * clear. Call after a successful cli_session_create so every teardown path (error
 * or normal) removes the home. No-op-safe when the seat uses the shared HOME. */
void cli_session_set_isolated_home(cli_session_t *s, const char *home);

/* Kills the tmux session. No-op if already dead. */
void cli_session_destroy(cli_session_t *s);

/* Returns 1 if the tmux session still exists, 0 otherwise. */
int cli_session_is_alive(const cli_session_t *s);

/* --- Send/Receive --- */
/* Sends message to the CLI via tmux paste-buffer + Enter.
 * Returns 0 on success. */
int cli_session_send(cli_session_t *s, const char *message);

/* Captures the current tmux pane contents immediately.
 * Writes captured text to out (NUL-terminated). Returns 0 on success,
 * -1 on error. */
int cli_session_capture(cli_session_t *s, char *out, size_t out_max);

/* Polls capture-pane until output stabilises (hash-based), the session dies,
 * or timeout_ms elapses. Writes captured text to out (NUL-terminated).
 * Returns 0 on success, -1 if the session exits before output stabilises,
 * -2 if it did not stabilise within timeout_ms, -3 if the cancel-check fired
 * (the turn was asked to stop), and -4 if the CLI sat in a provider error/retry
 * state past the error grace (cli_session_set_error_grace_ms). timeout_ms <= 0
 * disables
 * the wall-clock bound (legacy unbounded behaviour). The bound is the only
 * thing that breaks a CLI wedged in a provider retry loop (e.g. an Anthropic
 * outage), whose pane animates forever without the session dying. recv writes
 * ONLY this turn's clean response (pane diffed vs the baseline, TUI chrome
 * stripped) and streams the reply's growth via the thread's stream callback. */
int cli_session_recv(cli_session_t *s, char *out, size_t out_max, int timeout_ms);

/* Extract the latest assistant turn's clean text from a raw pane capture.
 * `cli_kind` selects the TUI markers ("claude"/"claude-code" use ●/❯/✻,
 * "codex" uses •/›). `baseline` is the pre-send pane snapshot (may be NULL);
 * content present in the baseline is excluded so prior turns never leak.
 * Returns newly allocated text; caller frees. Exposed for unit tests. */
char *cli_session_extract_response(const char *raw, const char *cli_kind, const char *baseline);

/* --- Session name helpers --- */
/* Build a deterministic session name: "aimee-<agent>-<hash(role)>".
 * Caller must free returned string. */
char *cli_session_make_name(const char *agent_name, const char *role);

/* Resolve the tmux execution name and reuse policy from the runtime identity.
 * Delegations are always isolated, even when they run outside a bound web
 * session; primary turns may reuse only their explicitly scoped pane. */
char *cli_session_make_execution_name(const char *agent_name, int configured_reuse,
                                      const char *session_id, int session_override,
                                      const char *delegation_id, int force_isolation,
                                      int *reuse_out);

/* --- Parser --- */
/* Extract plain text response from raw CLI terminal capture.
 * Strips ANSI escapes and leading/trailing whitespace.
 * Returns newly allocated string; caller must free. */
char *cli_session_strip_ansi(const char *raw);

/* Returns the newly appended output between two captured panes.
 * If the new capture does not extend the old one, returns the full current
 * capture as the safest fallback. Caller must free. */
char *cli_session_delta(const char *previous, const char *current);

#endif /* CLI_SESSION_H */
