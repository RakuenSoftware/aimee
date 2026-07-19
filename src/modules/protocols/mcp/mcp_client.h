/*
 * mcp_client.h: MCP (Model Context Protocol) client.
 *
 * Scope of this module (per the `mcp-client-transports` proposal, stdio slice):
 *
 *   - A transport abstraction (function-pointer table) so session code stays
 *     transport-agnostic. Today: stdio. SSE transport is a follow-up.
 *   - JSON-RPC 2.0 request/response framing suitable for MCP's `initialize`,
 *     `tools/list`, and `tools/call` verbs.
 *   - A session that performs the handshake, caches discovered tool schemas,
 *     and dispatches tool calls.
 *
 * Config parsing of `mcp_clients` and the tool-registry merge live in follow-up
 * proposals; this module just exposes the primitives those will layer on top of.
 */
#ifndef DEC_MCP_CLIENT_H
#define DEC_MCP_CLIENT_H 1

#include <stddef.h>

#include "cJSON.h"

typedef enum
{
   MCP_TRANSPORT_STDIO = 1,
   MCP_TRANSPORT_SSE = 2
} mcp_transport_kind_t;

/* Transport vtable. All three function pointers are required.
 *
 *   send: write a single JSON-RPC frame (newline-delimited JSON, no embedded
 *         newlines). Returns 0 on success, -1 on failure.
 *   recv: block up to |timeout_ms| for one complete frame. Writes up to
 *         |buflen - 1| bytes and NUL-terminates. Returns bytes written
 *         (excluding NUL), 0 on timeout, -1 on error.
 *   close: release transport resources. Safe to call on a partially-initialised
 *          transport (e.g. after send/recv failure).
 */
typedef struct mcp_transport mcp_transport_t;

typedef struct
{
   int (*send)(mcp_transport_t *t, const char *json, size_t len);
   int (*recv)(mcp_transport_t *t, char *buf, size_t buflen, int timeout_ms);
   void (*close)(mcp_transport_t *t);
} mcp_transport_vtable_t;

struct mcp_transport
{
   mcp_transport_kind_t kind;
   const mcp_transport_vtable_t *vt;
   void *state; /* transport-private */
};

/* Open a stdio transport: fork/exec |argv[0]| with |argv|; stdin and stdout
 * are connected to pipes. |argv| must be NULL-terminated. |cwd| may be NULL.
 *
 * The child's stderr is inherited so crashes surface in the parent's logs.
 *
 * Returns a heap-allocated transport, or NULL on failure. On success the caller
 * owns the transport and must release it via mcp_transport_close(). */
mcp_transport_t *mcp_transport_stdio_open(const char *const argv[], const char *cwd);

/* Open an SSE transport backed by:
 *   GET  |url|                -> SSE event stream (endpoint + message events)
 *   POST <endpoint event url> -> newline-delimited JSON-RPC frames
 *
 * |bearer_token| may be NULL. When set, every HTTP request sends
 * "Authorization: Bearer <token>".
 *
 * Returns a heap-allocated transport, or NULL on failure. */
mcp_transport_t *mcp_transport_sse_open(const char *url, const char *bearer_token);

/* Release a transport created by any transport opener. Safe on NULL. */
void mcp_transport_close(mcp_transport_t *t);

/* --- JSON-RPC framing ------------------------------------------------------- */

/* Build a JSON-RPC 2.0 request as a newline-terminated string.
 *
 *   {"jsonrpc":"2.0","id":<id>,"method":"<method>","params":<params-or-omitted>}\n
 *
 * |params| is borrowed; caller retains ownership. If |params| is NULL, the
 * "params" key is omitted.
 *
 * Returns a heap-allocated NUL-terminated string (caller frees) or NULL on
 * allocation failure. */
char *mcp_jsonrpc_build_request(int id, const char *method, const cJSON *params);

/* Parse a JSON-RPC response. On success, writes the matched id into |*out_id|
 * and returns the "result" subtree (borrowed from |*out_root|; caller must
 * cJSON_Delete(*out_root)). On error responses returns -1 and, if |err_buf|
 * is non-NULL, copies the server error message into it.
 *
 * Returns 0 on success (result populated), -1 on protocol error / error
 * response. */
int mcp_jsonrpc_parse_response(const char *frame, int *out_id, cJSON **out_root, cJSON **out_result,
                               char *err_buf, size_t err_buf_len);

/* --- Session ---------------------------------------------------------------- */

typedef struct
{
   char *name;                 /* client name, e.g. "github". Owned. */
   mcp_transport_t *transport; /* owned */
   cJSON *tool_schemas;        /* cached tools/list result; owned. May be NULL. */
   int next_id;
   int initialized;
} mcp_client_session_t;

/* Initialise a session wrapping |transport|. Takes ownership of |transport|
 * (released on mcp_client_session_close). |name| is copied.
 *
 * Returns 0 on success, -1 on alloc failure. */
int mcp_client_session_init(mcp_client_session_t *s, const char *name, mcp_transport_t *transport);

/* Perform the MCP `initialize` handshake. Safe to call once per session.
 * Returns 0 on success, -1 on protocol or transport failure. */
int mcp_client_initialize(mcp_client_session_t *s, int timeout_ms);

/* Fetch the tools/list result and cache it in |s->tool_schemas|.
 * Returns 0 on success, -1 on failure. */
int mcp_client_list_tools(mcp_client_session_t *s, int timeout_ms);

/* Invoke a tool. |args| is borrowed. On success, |*out_result| receives a
 * heap cJSON value (caller deletes). Returns 0 on success, -1 on failure;
 * error text written to |err_buf| if non-NULL. */
int mcp_client_call_tool(mcp_client_session_t *s, const char *tool, const cJSON *args,
                         int timeout_ms, cJSON **out_result, char *err_buf, size_t err_buf_len);

/* Release session resources. Safe on a zero-initialised session. */
void mcp_client_session_close(mcp_client_session_t *s);

#endif /* DEC_MCP_CLIENT_H */
