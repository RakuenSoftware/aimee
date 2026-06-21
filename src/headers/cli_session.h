#ifndef CLI_SESSION_H
#define CLI_SESSION_H

#include <time.h>
#include <stddef.h>

#define CLI_SESSION_NAME_MAX 128
#define CLI_SESSION_CMD_MAX  256
#define CLI_SESSION_BUF_MAX  (256 * 1024)
#define CLI_SESSION_POLL_MS  500 /* poll interval for completion detection */
#define CLI_SESSION_STABLE_N 3   /* consecutive stable polls to declare done */

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
} cli_session_t;

/* Incremental stream callback: invoked by cli_session_recv with each newly
 * produced chunk of clean response text as the turn streams. Set per-thread. */
typedef void (*cli_session_stream_cb_t)(const char *delta, void *ud);
void cli_session_set_stream_cb(cli_session_stream_cb_t cb, void *ud);
/* Read the current thread's stream callback so a caller can save/restore it
 * around a nested turn (prevents the inner turn's now-dead context leaking to
 * the outer turn). *ud_out receives the userdata; returns the callback. */
cli_session_stream_cb_t cli_session_get_stream_cb(void **ud_out);

/* Record the CLI kind so recv can pick the right TUI response parser. */
void cli_session_set_kind(cli_session_t *s, const char *cli_kind);

/* Snapshot the current pane as the baseline for the next recv. Call right
 * before cli_session_send so the turn's reply is diffed from a clean boundary. */
void cli_session_mark_baseline(cli_session_t *s);

/* --- Lifecycle --- */
/* Creates (or attaches to existing) tmux session, starts CLI.
 * session_name must be unique (e.g. "aimee-<agent>-<taskid>").
 * Returns 0 on success, -1 on failure. */
int cli_session_create(cli_session_t *s, const char *session_name, const char *cli_cmd,
                       const char *work_dir, int reuse);

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
 * and -2 if it did not stabilise within timeout_ms. timeout_ms <= 0 disables
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
