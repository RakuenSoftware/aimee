/* openai_shape.h: OpenAI-compatible JSON shaping helpers (no sockets, no network). */
#ifndef DEC_OPENAI_SHAPE_H
#define DEC_OPENAI_SHAPE_H

#include <stddef.h>

struct cJSON;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Forward declarations keep this low-level shaping header decoupled from the
    * agent pipeline and the server HTTP layer — it must not pull those headers
    * in (the original transitive include also had a fragile load-bearing order:
    * aimee.h before agent_protocol.h for the MAX_PATH_LEN macro). The .c units
    * that define or call the P2c helper below (openai_shape.c, openai_chat.c,
    * server_http.c) include the full definitions themselves.
    *   - parsed_response_t: forward-declared by tag only (the full
    *     `struct parsed_response` definition lives in agent_protocol.h and must
    *     be visible in any TU that *dereferences* it — callers passing the
    *     pointer through need only this forward decl).
    *   - openai_sse_emit_fn: signature-compatible with server_http_sse_event_emit;
    *     a distinct name avoids depending on server_http.h here.
    * DO NOT re-add #include "aimee.h" / "server_http.h" / "agent_protocol.h"
    * here — that re-introduces the inverted dependency and load-bearing order
    * this forward-decl block was created to remove. */
   typedef struct parsed_response parsed_response_t;
   typedef void (*openai_sse_emit_fn)(void *ctx, const char *event, const char *data_json);

   /* Parse an OpenAI /v1/chat/completions request body. Copies the model id into
    * model[model_n] (defaults to "aimee" if absent/empty), flattens messages[]
    * into a newline-joined "role: content" transcript heap-allocated into
    * *prompt_out (caller frees), and sets *stream_out to 1 if "stream":true.
    * Returns 0 on success, -1 if body is not valid JSON or has no message with
    * non-empty string content. On failure *prompt_out is set to NULL. */
   int openai_parse_chat_request(const char *body, char *model, size_t model_n, char **prompt_out,
                                 int *stream_out);

   /* Parse an OpenAI /v1/completions request body ({model, prompt, stream}).
    * Copies model into model[model_n] (default "aimee"), strdups the "prompt"
    * string into *prompt_out (caller frees), sets *stream_out. Returns 0 on
    * success, -1 on invalid JSON or missing/empty prompt. *prompt_out NULL on
    * failure. */
   int openai_parse_completion_request(const char *body, char *model, size_t model_n,
                                       char **prompt_out, int *stream_out);

   /* Parse an OpenAI /v1/responses request body
    * ({model, input, previous_response_id, stream}). `input` may be a string or
    * an array of items, each either a bare string or {role, content} where
    * content is a string or an array of {type, text} parts; all text is
    * flattened into a heap "role: text\n" transcript in *prompt_out (caller
    * frees). Copies model (default "aimee") and previous_response_id (empty if
    * absent) into the given buffers, sets *stream_out. Returns 0 on success, -1
    * on invalid JSON or empty input. *prompt_out NULL on failure. */
   int openai_parse_responses_request(const char *body, char *model, size_t model_n,
                                      char **prompt_out, char *prev_id, size_t prev_id_n,
                                      int *stream_out);

   /* Convert a Codex/OpenAI Responses request into the OpenAI chat shape for the
    * full-parity ingress: copies `model` (default "aimee"), strdup's
    * `instructions` into *instructions_out (NULL if absent; caller frees),
    * builds *messages_out as a chat messages array (message / function_call ->
    * assistant tool_calls / function_call_output -> role:tool), and *tools_out as
    * chat function tools (NULL if none; only `function`-type tools kept). Sets
    * *stream_out. Returns 0 on success, -1 on invalid JSON. On success the caller
    * owns and cJSON_Delete()s *messages_out and *tools_out. */
   int openai_parse_responses_to_chat(const char *body, char *model, size_t model_n,
                                      char **instructions_out, struct cJSON **messages_out,
                                      struct cJSON **tools_out, int *stream_out);

   /* Build an OpenAI Responses object into resp[cap]:
    * {"id":…,"object":"response","created_at":…,"model":…,"status":"completed",
    *  "output":[{"id":…,"type":"message","status":"completed","role":"assistant",
    *  "content":[{"type":"output_text","text":…,"annotations":[]}]}],
    *  "usage":{"input_tokens":…,"output_tokens":…,"total_tokens":…}}
    * Returns bytes written (excluding NUL), or -1 if it does not fit. */
   int openai_format_response(const char *id, const char *model, const char *output_text,
                              long created, int prompt_tokens, int completion_tokens, char *resp,
                              int cap);

   /* Build a `run` object (same message/usage shape as a response, with
    * "object":"run" and the given status, e.g. "completed"). Returns bytes
    * written (excluding NUL), or -1 if it does not fit. */
   int openai_format_run(const char *id, const char *model, const char *output_text, long created,
                         int prompt_tokens, int completion_tokens, const char *status, char *resp,
                         int cap);

   /* Responses API streaming events (data payloads; the caller writes the SSE
    * `event:` line and frames them). Each returns bytes written or -1.
    *  created   → {"type":"response.created","response":{…status:in_progress…}}
    *  delta     → {"type":"response.output_text.delta","item_id":…,
    *               "output_index":0,"content_index":0,"delta":<text>}
    *  completed → {"type":"response.completed","response":{…full…}} */
   int openai_format_responses_created(const char *id, const char *model, long created, char *resp,
                                       int cap);
   int openai_format_responses_delta(const char *item_id, const char *delta, char *resp, int cap);
   int openai_format_responses_completed(const char *id, const char *model, const char *output_text,
                                         long created, int prompt_tokens, int completion_tokens,
                                         char *resp, int cap);

   /* Responses-API terminal error events (Codex parity). Codex treats both as
    * fatal stream errors:
    *  failed     → {"type":"response.failed","response":{…status:"failed",
    *               "error":{"code":…,"message":…}}}. Codex maps `code` to a
    *               typed ApiError (context_length_exceeded, insufficient_quota,
    *               cyber_policy, invalid_prompt, server_is_overloaded/slow_down,
    *               rate_limit_exceeded→retry-after); any other code → Retryable.
    *  incomplete → {"type":"response.incomplete","response":{…status:"incomplete",
    *               "incomplete_details":{"reason":…}}} (e.g. reason
    *               "max_output_tokens" on truncation). Returns bytes written or -1. */
   int openai_format_responses_failed(const char *id, const char *model, long created,
                                      const char *code, const char *message, char *resp, int cap);
   int openai_format_responses_incomplete(const char *id, const char *model, long created,
                                          const char *reason, char *resp, int cap);

   /* Responses-API output items + item/argument streaming events (Codex parity).
    * Codex requires output_item.added before any text/arguments delta, and
    * output_item.done + completed carrying the items. The *_item builders return
    * a new cJSON object (caller owns / adds to an output array). The format_*
    * helpers emit the matching SSE data payloads (bytes written or -1). */
   struct cJSON *openai_responses_message_item(const char *item_id, const char *text,
                                               const char *status);
   struct cJSON *openai_responses_function_call_item(const char *item_id, const char *call_id,
                                                     const char *name, const char *arguments,
                                                     const char *status);
   int openai_format_responses_msg_item_added(const char *item_id, int output_index, char *resp,
                                              int cap);
   int openai_format_responses_msg_item_done(const char *item_id, const char *text,
                                             int output_index, char *resp, int cap);
   int openai_format_responses_fc_item_added(const char *item_id, const char *call_id,
                                             const char *name, int output_index, char *resp,
                                             int cap);
   int openai_format_responses_fc_item_done(const char *item_id, const char *call_id,
                                            const char *name, const char *arguments,
                                            int output_index, char *resp, int cap);
   int openai_format_responses_fc_args_delta(const char *item_id, int output_index,
                                             const char *delta, char *resp, int cap);
   int openai_format_responses_fc_args_done(const char *item_id, int output_index,
                                            const char *arguments, char *resp, int cap);
   int openai_format_responses_completed_items(const char *id, const char *model, long created,
                                               struct cJSON *output_arr, int prompt_tokens,
                                               int completion_tokens, char *resp, int cap);

   /* Read an optional numeric sampling field from an OpenAI request body.
    * Returns the value when it is a finite number within [0, hi]; otherwise
    * (absent, non-numeric, out of range, or invalid JSON) returns dflt. */
   double openai_request_double(const char *body, const char *field, double dflt, double hi);

   /* Read an optional integer field (e.g. max_tokens). Returns the value when it
    * is an integer within [1, hi]; otherwise returns dflt. */
   int openai_request_int(const char *body, const char *field, int dflt, int hi);

   /* Read an optional boolean field (e.g. stream). Returns 1 only when the
    * field is present and JSON true; 0 otherwise (absent/false/non-bool/invalid). */
   int openai_request_bool(const char *body, const char *field);

   /* Build one streaming chat.completion.chunk frame into resp[cap]:
    * {"id":…,"object":"chat.completion.chunk","created":…,"model":…,
    *  "choices":[{"index":0,"delta":{…},"finish_reason":<null|"stop">}]}
    * role!=0 adds "role":"assistant" to the delta (first frame);
    * delta_content!=NULL adds "content":<delta>; finish!=0 sets
    * finish_reason:"stop" (terminal frame) else null. Returns bytes written
    * (excluding NUL), or -1 if it does not fit. */
   int openai_format_chat_chunk(const char *id, const char *model, long created, int role,
                                const char *delta_content, int finish, char *resp, int cap);

   /* Build one legacy streaming text_completion chunk into resp[cap]:
    * {"id":…,"object":"text_completion","created":…,"model":…,
    *  "choices":[{"text":<delta>,"index":0,"finish_reason":<null|"stop">}]}
    * finish!=0 sets finish_reason:"stop" (terminal frame, text usually "").
    * Returns bytes written (excluding NUL), or -1 if it does not fit. */
   int openai_format_text_chunk(const char *id, const char *model, long created,
                                const char *text_delta, int finish, char *resp, int cap);

   /* Parse an OpenAI /v1/embeddings request body ({model, input}). `input` may
    * be a single string or an array of strings. Copies model into
    * model[model_n] (default "aimee") and allocates an array of *n_out heap
    * strings into *inputs_out. Returns 0 on success, -1 on invalid JSON or
    * missing/empty input. On failure *inputs_out is NULL and *n_out is 0. Free
    * with openai_free_inputs. */
   int openai_parse_embeddings_request(const char *body, char *model, size_t model_n,
                                       char ***inputs_out, int *n_out);

   /* Free an inputs array returned by openai_parse_embeddings_request. */
   void openai_free_inputs(char **inputs, int n);

   /* Build an OpenAI embeddings response into resp[cap]:
    * {"object":"list","data":[{"object":"embedding","index":i,"embedding":[…]}…],
    *  "model":model,"usage":{"prompt_tokens":pt,"total_tokens":pt}}
    * vecs[i] holds dims[i] floats. Returns bytes written, or -1. */
   int openai_format_embeddings(const char *model, const float *const *vecs, const int *dims, int n,
                                int prompt_tokens, char *resp, int cap);

   /* Build {"object":"list","data":[{"id":<id>,"object":"model","owned_by":<owner>}]}
    * into resp[cap]. ids is an array of n model-id strings. Returns the byte
    * length written (excluding NUL), or -1 on error. */
   int openai_format_models_list(const char *const *ids, int n, const char *owner, char *resp,
                                 int cap);

   /* Build a chat.completion object into resp[cap]:
    * {"id":id,"object":"chat.completion","created":created,"model":model,
    *  "choices":[{"index":0,"message":{"role":"assistant","content":content},
    *  "finish_reason":"stop"}],
    *  "usage":{"prompt_tokens":pt,"completion_tokens":ct,"total_tokens":pt+ct}}
    * Returns bytes written, or -1. */
   int openai_format_chat_completion(const char *id, const char *model, const char *content,
                                     long created, int prompt_tokens, int completion_tokens,
                                     char *resp, int cap);

   /* Build a legacy text_completion object into resp[cap]:
    * {"id":id,"object":"text_completion","created":created,"model":model,
    *  "choices":[{"text":content,"index":0,"finish_reason":"stop"}],
    *  "usage":{"prompt_tokens":pt,"completion_tokens":ct,"total_tokens":pt+ct}}
    * Returns bytes written, or -1. */
   int openai_format_text_completion(const char *id, const char *model, const char *content,
                                     long created, int prompt_tokens, int completion_tokens,
                                     char *resp, int cap);

   /* Build an OpenAI error envelope {"error":{"message":msg,"type":type}} into
    * resp[cap]. Returns bytes written, or -1. */
   int openai_format_error(char *resp, int cap, const char *type, const char *message);

   /* As openai_format_error, but also stamps an aimee-specific error code
    * (>=1000, see aimee_errors.h) into error.code and logs it when aimee_code>0,
    * for aimee-internal faults (no primary, routing/credentials). The wire HTTP
    * status is still chosen by the caller's return value. aimee_code==0 is
    * identical to openai_format_error. */
   int openai_format_error_code(char *resp, int cap, const char *type, const char *message,
                                int aimee_code);

   /* P2c (response-side tool policing, OpenAI streaming): emit the tool-call
    * tail of the `response.*` SSE sequence for a parsed_response_t that carries
    * `calls[]` (the post-police shape — calls[] may be empty if every entry was
    * a denied subagent). The wire shape mirrors the existing tool-call emit loop
    * in responses_stream_handler; this helper exists for unit-testability
    * (test_openai_chat_policed.c).
    *
    * Caller contract: the caller MUST emit the leading `response.created`
    * envelope event exactly once before invoking this helper (every path —
    * text, tool-call, error — shares that single emit). This helper does NOT
    * emit `response.created`, so the tool-call path never doubles it on the wire.
    * - If parsed->call_count > 0: emits per-call frames for each surviving
    *   call (output_item.added, function_call_arguments.delta/.done,
    *   output_item.done), then response.completed with all calls in output[].
    * - If parsed->call_count == 0 (the P2c all-dropped case), OR parsed is
    *   NULL: emits a single response.completed with empty output[] (a NULL
    *   parsed is treated as zero calls / zero usage, never dereferenced).
    *
    * Ownership / lifetime:
    * - The function borrows everything; it takes ownership of nothing and frees
    *   nothing of the caller's. `parsed`, `id`, `model`, and the `ctx` behind
    *   `emit` need only outlive the call (synchronous — `emit` is invoked only
    *   before this function returns; neither `emit` nor `ctx` is retained).
    * - A NULL `emit` is tolerated (the helper returns without emitting) — a guard
    *   for the public/test surface; the production caller (responses_stream_handler)
    *   always passes a real emit and has already emitted `response.created`
    *   through it before calling, so a NULL there is a non-starter. Per-call and
    *   `response.completed` frames use scratch the function mallocs (sized to the
    *   id/model/args) and frees internally — no caller scratch is needed.
    * - Emit failures are not the helper's concern: `emit` returns void and the
    *   helper does not roll back or resume a partially-emitted sequence. A
    *   transport that can fail mid-stream must detect it inside `emit`/`ctx`;
    *   the helper always walks the full sequence. */
   void openai_responses_emit_policed(const parsed_response_t *parsed, const char *id,
                                      const char *model, long created, openai_sse_emit_fn emit,
                                      void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DEC_OPENAI_SHAPE_H */