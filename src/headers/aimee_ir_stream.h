/* aimee_ir_stream.h -- Slice 4: the neutral IR-DELTA streaming model. A BACKEND SSE
 * parser turns a provider stream into IR delta events; a FRONTEND SSE renderer
 * turns IR deltas into the client's stream. Together they replace the direct
 * provider-SSE -> client-SSE translators (anthropic_stream_feed_openai / xlate_*).
 * Pure state machines (no I/O); deltas borrow into the parsed chunk (transient). */
#ifndef DEC_AIMEE_IR_STREAM_H
#define DEC_AIMEE_IR_STREAM_H 1

#include <stddef.h>

#include "aimee_ir.h"

struct cJSON;

#define AIMEE_STREAM_MAX_TOOLS 64

/* --- backend: OpenAI Chat Completions SSE chunk -> IR deltas --- */
typedef struct
{
   int started;                            /* TURN_START emitted */
   int text_block;                         /* block id of the open text block, or -1 */
   int next_block;                         /* next block id to assign */
   int tool_block[AIMEE_STREAM_MAX_TOOLS]; /* openai tool_calls[i].index -> block id (-1 = none) */
   int stopped;                            /* TURN_STOP emitted */
} openai_stream_state_t;

void openai_stream_state_init(openai_stream_state_t *st);

/* Convert one parsed OpenAI-chat SSE chunk into up to `max` IR deltas (updates st).
 * Returns the number written (>=0). Text + tool_calls + finish_reason handled. */
int openai_chunk_to_deltas(const struct cJSON *chunk, openai_stream_state_t *st, aimee_delta_t *out,
                           int max);

/* --- frontend: IR delta -> Anthropic Messages SSE text --- */
typedef struct
{
   int started; /* message_start emitted */
} anthropic_stream_state_t;

/* Render one IR delta as Anthropic SSE event text (malloc'd `event: ...\ndata:
 * ...\n\n`, caller frees), or NULL if the delta yields no output. msg_id + model
 * are used on the first (TURN_START) event. */
char *anthropic_delta_render(const aimee_delta_t *d, anthropic_stream_state_t *st,
                             const char *msg_id, const char *model);

/* Split (ctx, event, data_json) SSE sink -- signature-compatible with the
 * server's server_http_sse_event_emit, so the live relay's emit is passed
 * directly. */
typedef void (*aimee_sse_emit_fn)(void *ctx, const char *event, const char *data_json);

/* Like anthropic_delta_render, but emits each Anthropic SSE event via `emit`
 * (event name + data JSON, unframed) instead of returning a framed string --
 * matches the live SSE relay's split emit sink. TURN_STOP emits two events
 * (message_delta + message_stop). Returns the number of events emitted. This is
 * the replacement for the legacy incremental translator (anthropic_stream_feed_
 * openai) on the live relay path. */
int anthropic_delta_emit(const aimee_delta_t *d, anthropic_stream_state_t *st, const char *msg_id,
                         const char *model, aimee_sse_emit_fn emit, void *ctx);

#endif /* DEC_AIMEE_IR_STREAM_H */
