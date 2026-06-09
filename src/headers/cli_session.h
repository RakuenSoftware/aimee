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
   int reuse;                               /* 1 = reuse across tasks */
   time_t last_activity;
   int active; /* 1 = session exists */
} cli_session_t;

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

/* Polls capture-pane until output stabilises (hash-based) or session dies.
 * Writes captured text to out (NUL-terminated). Returns 0 on success,
 * -1 if the session exits before output stabilises. */
int cli_session_recv(cli_session_t *s, char *out, size_t out_max);

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
