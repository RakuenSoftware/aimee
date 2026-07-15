/* cli_chat_stream.h: headless /v1 chat streaming for non-interactive callers.
 *
 * Was cli_tui.h. The interactive terminal UIs it was named for — the OpenCode
 * frontend, the native fallback loop and `aimee chat` — are gone; nothing here
 * touches a terminal. acp-serve is the only caller. */
#ifndef DEC_CLI_CHAT_STREAM_H
#define DEC_CLI_CHAT_STREAM_H 1

/* Run `message` through aimee-server, streaming incremental output: text_cb(delta, ud)
 * fires per text chunk and tool_cb(phase, tool_name, ud) per tool event; either may be
 * NULL. Returns the full reply (heap, caller frees) or NULL on error. Writes nothing to
 * stdout, so callers owning a protocol stream stay clean. */
char *cli_chat_stream(const char *sock, const char *session_id, const char *message,
                      void (*text_cb)(const char *delta, void *ud),
                      void (*tool_cb)(const char *phase, const char *tool_name, void *ud),
                      void *ud);
#endif /* DEC_CLI_CHAT_STREAM_H */
