/* aimee_ir_stream.h -- Slice 4: the neutral IR-DELTA streaming model. A BACKEND SSE
 * parser turns a provider stream into IR delta events; a FRONTEND SSE renderer
 * turns IR deltas into the client's stream. Together they replace the direct
 * provider-SSE -> client-SSE translators (anthropic_stream_feed_openai / xlate_*).
 * Pure state machines (no I/O); deltas borrow into the parsed chunk (transient). */
#ifndef DEC_AIMEE_IR_STREAM_H
#define DEC_AIMEE_IR_STREAM_H 1

#include <stddef.h>

#include <aimee/ir/aimee_ir.h>

struct cJSON;

#define AIMEE_STREAM_MAX_TOOLS 64

/* --- backend: OpenAI Chat Completions SSE chunk -> IR deltas --- */
typedef struct
{
   int started;                            /* TURN_START emitted */
   int text_block;                         /* block id of the open text block, or -1 */
   int reasoning_block;                    /* block id of the open reasoning block, or -1 */
   int next_block;                         /* next block id to assign */
   int tool_block[AIMEE_STREAM_MAX_TOOLS]; /* openai tool_calls[i].index -> block id (-1 = none) */
   int stopped;                            /* TURN_STOP emitted */
} openai_stream_state_t;

void openai_stream_state_init(openai_stream_state_t *st);

/* Convert one parsed OpenAI-chat SSE chunk into up to `max` IR deltas (updates st).
 * Returns the number written (>=0). Text + reasoning + tool_calls + finish_reason
 * handled. Reasoning is read from `delta.reasoning_content` (the DeepSeek / vLLM /
 * llama.cpp spelling) or `delta.reasoning` (the OpenRouter spelling) -- OpenAI's own
 * API exposes no reasoning text on this wire, so a chunk with neither simply yields
 * no THINKING deltas. Reasoning precedes content in these streams and is emitted as
 * its own block, so a consumer never has to separate the two by position. */
int openai_chunk_to_deltas(const struct cJSON *chunk, openai_stream_state_t *st, aimee_delta_t *out,
                           int max);

/* --- backend: AWS Bedrock ConverseStream event -> IR deltas ---
 * The streaming analogue of bedrock_converse_parse: maps ONE decoded ConverseStream
 * event (its `:event-type` header string + the already-parsed payload JSON) to 0+ IR
 * deltas. NO eventstream framing here (P6b decodes the binary frame upstream, kb-side)
 * -- this is pure JSON -> deltas, server-side. Per-contentBlockIndex block KIND is
 * tracked (bounded like the openai state) ONLY so a contentBlockStop carries the right
 * `kind`. */
typedef struct
{
   aimee_block_type_t kind[AIMEE_STREAM_MAX_TOOLS]; /* per contentBlockIndex block kind */
   int kind_set[AIMEE_STREAM_MAX_TOOLS];            /* 1 once kind[i] recorded */
   int message_stop_seen;
   int terminal_emitted;
   aimee_stop_reason_t pending_stop_reason;
} converse_stream_state_t;

void converse_stream_state_init(converse_stream_state_t *st);

/* Convert one decoded ConverseStream event into up to `max` IR deltas (updates st).
 * The first text/reasoning delta yields BLOCK_START plus BLOCK_DELTA, so callers
 * must provide capacity >=2 for content events.
 * `event_type` is the frame's `:event-type` header (e.g. "contentBlockDelta"); payload
 * is the decoded event JSON. Returns the number of deltas written (>=0, <=max), or -1
 * on a structurally-malformed KNOWN event (caller drops the stream). An unknown
 * event_type -> 0 (forward-compat ignore). LIFETIME: like openai_chunk_to_deltas, the
 * emitted deltas' const char* fields (text_delta / tool_args_delta / tool_id /
 * tool_name / error_message) BORROW into `payload` (or, for an exception event with no
 * `message`, into `event_type`); both `payload` and `event_type` must outlive the
 * deltas' use. An out-of-range contentBlockIndex on a contentBlock* event returns -1
 * (a valid Converse stream never emits one) so no unbounded block_id escapes. */
int bedrock_converse_stream_to_deltas(const char *event_type, const struct cJSON *payload,
                                      converse_stream_state_t *st, aimee_delta_t *out, int max);

/* --- backend: Anthropic Messages SSE event -> IR deltas ---
 * The cell the backend matrix was missing: Anthropic appeared here only as a
 * FRONTEND renderer, so an Anthropic-speaking provider had no way onto the neutral
 * delta model and its stream could only be relayed verbatim. This is the streaming
 * counterpart of anthropic_backend_parse, and the mirror of anthropic_delta_render.
 *
 * `event_type` is the SSE `event:` name (e.g. "content_block_delta"); payload is the
 * parsed `data:` JSON. Unlike the Converse parser, EVERY event yields at most ONE
 * delta -- Anthropic always sends content_block_start before any delta, so no
 * implicit BLOCK_START is ever synthesised -- so capacity >=1 suffices.
 *
 * KNOWN GAPS, deliberately not papered over here:
 *   - `signature_delta` yields 0 deltas. aimee_delta_t has no signature field, and
 *     giving it one would not help: a delta BORROWS into its event's payload, which
 *     is gone by the time the block closes, so the signature cannot be carried to
 *     the BLOCK_STOP it describes without copying into the state machine. The
 *     NON-streaming path does model it (aimee_block_t.thinking_signature, captured
 *     by anthropic_backend_parse); a stream reassembled through these deltas alone
 *     is therefore not resubmittable.
 *   - the frontend renderer flattens AIMEE_BLK_THINKING to a `text` block, so a
 *     thinking delta parsed here does not round-trip as thinking on egress.
 * Both are safe for OBSERVING a stream (the relay emits provider bytes verbatim);
 * neither is safe for translating one. */
typedef struct
{
   aimee_block_type_t kind[AIMEE_STREAM_MAX_TOOLS]; /* per content-block index, block kind */
   int kind_set[AIMEE_STREAM_MAX_TOOLS];            /* 1 once kind[i] recorded */
   int started;                                     /* message_start seen */
   int terminal_emitted;                            /* TURN_STOP emitted */
   aimee_stop_reason_t pending_stop_reason;         /* from message_delta */
   long pending_usage_in;                           /* from message_start */
   long pending_usage_out;                          /* from message_delta */
} anthropic_backend_stream_state_t;

void anthropic_backend_stream_state_init(anthropic_backend_stream_state_t *st);

/* Convert one Anthropic SSE event into up to `max` IR deltas (updates st). Returns
 * the number written (>=0, <=max), or -1 on a structurally-malformed KNOWN event
 * (caller drops the stream). An unknown event_type -> 0 (forward-compat ignore), as
 * do `ping` and a not-yet-modelled content_block_delta variant. LIFETIME: like the
 * other backend parsers, the emitted deltas' const char* fields BORROW into `payload`
 * (or, for an error event with no message, into a string literal); `payload` must
 * outlive the deltas' use. An out-of-range content-block index returns -1, so no
 * unbounded block_id escapes. Terminal shape: message_delta stashes stop_reason +
 * output_tokens and yields 0; message_stop emits the single TURN_STOP carrying them. */
int anthropic_stream_to_deltas(const char *event_type, const struct cJSON *payload,
                               anthropic_backend_stream_state_t *st, aimee_delta_t *out, int max);

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
