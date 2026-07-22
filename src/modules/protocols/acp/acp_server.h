/* acp_server.h: inbound Agent Client Protocol (ACP) server — `aimee acp-serve`.
 *
 * aimee speaks a simplified newline-delimited JSON-RPC 2.0 ACP dialect on stdio
 * (mirroring the outbound client in src/server/cli_acp.c), so ACP editors (Zed,
 * Aider's editor mode, …) can use aimee as a backend. Phase 1 implements the
 * stdio dispatch loop and the `initialize` handshake; session/new and
 * prompt/submit turn dispatch are later phases. */
#ifndef DEC_ACP_SERVER_H
#define DEC_ACP_SERVER_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Pure JSON-RPC request dispatch (no I/O — unit-testable). Parses one
    * newline-delimited JSON-RPC message in `line` and, when a reply is due,
    * writes the response JSON (no trailing newline) into `out`.
    * Returns: 1 when `out` holds a response to send; 0 when there is nothing to
    * send (a notification, or a blank line); the response is also 1 for
    * JSON-RPC errors (parse error / invalid request / method-not-found), which
    * carry an `error` object. `out` is always NUL-terminated when out_n > 0. */
   int acp_server_handle_line(const char *line, char *out, size_t out_n);

   /* Build the `initialize` result response for request id `id_json` (the raw
    * JSON text of the request's "id", or NULL for a null id). Pure. Returns 1 on
    * success (response in `out`), -1 on error. */
   int acp_server_build_initialize(const char *id_json, char *out, size_t out_n);

   /* Turn-dispatch callback: run a prompt through aimee and return the reply
    * text (heap, caller frees), or NULL on error. `session_id` may be NULL.
    * Injected by the caller so acp_server.c stays free of server/chat deps
    * (and unit-testable in isolation). */
   typedef char *(*acp_prompt_dispatch_fn)(const char *content, const char *session_id);

   /* If `line` is a `prompt/submit` request, extract its content, optional
    * sessionId, and the raw JSON of its id; returns 1 (buffers filled; id_json
    * is "" for a null/absent id, session_id is "" when absent). Returns 0 when
    * the line is not a prompt request (the caller falls back to
    * acp_server_handle_line). Returns 0 (not a prompt), 1 (legacy
    * `prompt/submit`, reply via result.content), or 2 (standard ACP
    * `session/prompt`, reply streamed + result.stopReason). Pure. */
   int acp_prompt_parse(const char *line, char *content, size_t content_n, char *session_id,
                        size_t session_n, char *id_json, size_t id_n);

   /* Build the legacy `prompt/submit` result `{result:{content:<reply>}}` for
    * id_json (raw id JSON or NULL). Pure. Returns 1 on success. */
   int acp_build_prompt_result(const char *id_json, const char *reply, char *out, size_t out_n);

   /* Build the standard ACP `session/prompt` response `{result:{stopReason}}`
    * (reply text already streamed via session/update). Pure. Returns 1. */
   int acp_build_prompt_stop(const char *id_json, const char *stop_reason, char *out, size_t out_n);

   /* If `content` is a slash command the server answers locally (currently
    * `/help`), write its reply text into `reply` and return 1; else return 0 so
    * the caller dispatches it as a normal turn. Pure. */
   int acp_slash_reply(const char *content, char *reply, size_t reply_n);

   /* Build a streaming `session/update` notification carrying one incremental
    * text chunk of the in-flight turn (no id — it's a notification). `session_id`
    * may be NULL/"". Pure. Returns 1 on success. */
   /* Build a standard ACP tool-call session/update notification. `phase` is
    * "started" or "completed". `tool_call_id` must be unique within the session --
    * it is how a client matches a tool_call_update to its tool_call; the tool NAME
    * is not unique and collapses repeat calls into one flickering entry.
    * `tool_name` is the human-facing title. Pure. Returns 1. */
   int acp_build_tool_update(const char *session_id, const char *tool_call_id, const char *phase,
                             const char *tool_name, char *out, size_t out_n);

   int acp_build_update_notification(const char *session_id, const char *delta, char *out,
                                     size_t out_n);

   /* Run the ACP server: read newline-delimited JSON-RPC from stdin; route
    * prompt/submit through `dispatch` (may be NULL → turns error out), other
    * methods through acp_server_handle_line; write responses to stdout (one per
    * line). Returns when stdin reaches EOF. */
   int acp_server_run(acp_prompt_dispatch_fn dispatch);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ACP_SERVER_H */
