#ifndef CLI_STREAM_SINK_H
#define CLI_STREAM_SINK_H 1

#include <stddef.h>

/* Per-turn streaming sink for local-CLI (provider-cli) agent output.
 *
 * When a CLI agent (e.g. claude -p) runs on a detached thin client, its stdout
 * is streamed back over the reverse channel chunk-by-chunk. The provider-cli
 * adapter parses each stream-json text delta and calls cli_stream_sink_emit()
 * so the chat turn can forward it into the live SSE stream — giving remote CLI
 * agents the same token-by-token UX as the HTTP-streaming primary path.
 *
 * The sink is THREAD-LOCAL (one per turn worker), installed at the chat-turn
 * boundary and cleared at the end, mirroring the agent_set_request_* /
 * g_rpc_conn_caps per-turn seams. No-op when no sink is installed (delegates
 * with no live SSE surface, the buffered/co-located path). */

typedef void (*cli_stream_emit_fn)(void *ctx, const char *text, size_t len);

/* Install the sink for the current thread/turn. */
void cli_stream_sink_set(cli_stream_emit_fn fn, void *ctx);

/* Clear the current thread's sink (call at turn end). */
void cli_stream_sink_clear(void);

/* True if a sink is installed on this thread. */
int cli_stream_sink_active(void);

/* Emit `len` bytes of assistant text to the installed sink (no-op if none). */
void cli_stream_sink_emit(const char *text, size_t len);

#endif /* CLI_STREAM_SINK_H */
