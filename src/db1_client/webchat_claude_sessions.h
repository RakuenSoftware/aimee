/* db1/webchat_claude_sessions.h: deployment-wide per-tab binding of the Claude CLI
 * session id used for `claude --resume`.
 *
 * The webchat Claude path resumes Claude Code's own on-disk session store by id
 * (claude --resume <id>). That id arrives from the browser, so a stale or
 * cross-wired client value could resume ANOTHER tab's conversation, merging two
 * chats at the model level. The binding is keyed by aimee_session_id across the
 * whole server environment. principal is retained only as actor attribution. */
#ifndef DEC_DB1_WEBCHAT_CLAUDE_SESSIONS_H
#define DEC_DB1_WEBCHAT_CLAUDE_SESSIONS_H 1

#include <stddef.h>

/* Fetch the Claude session id bound to aimee_session_id. Writes the
 * bound id into out (NUL-terminated) and returns 0 when a non-empty binding
 * exists; returns -1 (out set to "") otherwise. principal is ignored for
 * ownership and retained in the API for actor attribution compatibility. */
int db1_webchat_claude_session_get(const char *principal, const char *aimee_session_id, char *out,
                                   size_t out_n);

/* Returns 1 if claude_session_id is already bound to a DIFFERENT
 * aimee_session_id than the one given — i.e. adopting it for this
 * tab would steal another tab's Claude session. Returns 0 otherwise. */
int db1_webchat_claude_session_owned_by_other(const char *principal, const char *aimee_session_id,
                                              const char *claude_session_id);

/* Bind claude_session_id to aimee_session_id, upserting the tab's
 * row. No-op (returns -1) when claude_session_id is owned by a different tab, so
 * a binding can never be hijacked. Returns 0 on a successful bind/update. */
int db1_webchat_claude_session_bind(const char *principal, const char *aimee_session_id,
                                    const char *claude_session_id);

#endif
