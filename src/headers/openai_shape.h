/* openai_shape.h: OpenAI-compatible JSON shaping helpers (no sockets, no network). */
#ifndef DEC_OPENAI_SHAPE_H
#define DEC_OPENAI_SHAPE_H

#include <stddef.h>

struct cJSON;

#ifdef __cplusplus
extern "C"
{
#endif

   typedef void (*openai_sse_emit_fn)(void *ctx, const char *event, const char *data_json);

   /* Bounded provider diagnostic. Only recognized JSON error text is exposed;
    * never copy a raw HTML/proxy response or unrelated response fields. */
   void openai_upstream_error_message(int status, const char *body, char *out, size_t cap);

   /* Build the terminal chat.completion.chunk with an empty delta and an
    * explicit finish_reason ("stop", "tool_calls", …). openai_format_chat_chunk's
    * finish frame always says "stop"; a tool-call turn must not. Returns bytes
    * written (excluding NUL), or -1 if it does not fit. */
   int openai_format_chat_chunk_finish(const char *id, const char *model, long created,
                                       const char *reason, char *resp, int cap);

   /* Build an OpenAI Responses object into resp[cap]:
    * {"id":…,"object":"response","created_at":…,"model":…,"status":"completed",
    *  "output":[{"id":…,"type":"message","status":"completed","role":"assistant",
    *  "content":[{"type":"output_text","text":…,"annotations":[]}]}],
    *  "usage":{"input_tokens":…,"output_tokens":…,"total_tokens":…,
    *           "input_tokens_details":{"cached_tokens":…}}}
    *
    * cached_tokens is the part of prompt_tokens the provider served from its
    * prompt cache. Pass it through from the upstream reply -- do NOT pass 0 as a
    * placeholder on a path that has the real value. A client bills what this
    * block says, so a dropped cached count silently prices cache reads at the
    * full uncached rate (roughly 10x), and looks exactly like caching being
    * broken rather than unreported. Emitted even when zero.
    *
    * Returns bytes written (excluding NUL), or -1 if it does not fit. */
   int openai_format_response(const char *id, const char *model, const char *output_text,
                              long created, int prompt_tokens, int completion_tokens,
                              int cached_tokens, char *resp, int cap);
   /* As openai_format_response, but a non-NULL incomplete_reason (e.g.
    * "max_output_tokens") marks the reply status "incomplete" with
    * incomplete_details.reason, the way the Responses backend reports truncation. */
   int openai_format_response_status(const char *id, const char *model, const char *output_text,
                                     long created, int prompt_tokens, int completion_tokens,
                                     int cached_tokens, const char *incomplete_reason, char *resp,
                                     int cap);

   /* Build a `run` object (same message/usage shape as a response, with
    * "object":"run" and the given status, e.g. "completed"). Returns bytes
    * written (excluding NUL), or -1 if it does not fit. */
   int openai_format_run(const char *id, const char *model, const char *output_text, long created,
                         int prompt_tokens, int completion_tokens, int cached_tokens,
                         const char *status, char *resp, int cap);

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
                                         int cached_tokens, char *resp, int cap);

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
   /* `tool_namespace` is the call's owning namespace group, or NULL/"" when it has
    * none; it is emitted only when set. A Codex client offers its MCP tools inside
    * a `namespace` group and routes the answer on (namespace, name) together, so a
    * call that arrived with one must carry it back. */
   struct cJSON *openai_responses_function_call_item(const char *item_id, const char *call_id,
                                                     const char *name, const char *tool_namespace,
                                                     const char *arguments, const char *status);
   int openai_format_responses_msg_item_added(const char *item_id, int output_index, char *resp,
                                              int cap);
   int openai_format_responses_msg_item_done(const char *item_id, const char *text,
                                             int output_index, char *resp, int cap);
   int openai_format_responses_fc_item_added(const char *item_id, const char *call_id,
                                             const char *name, const char *tool_namespace,
                                             int output_index, char *resp, int cap);
   int openai_format_responses_fc_item_done(const char *item_id, const char *call_id,
                                            const char *name, const char *tool_namespace,
                                            const char *arguments, int output_index, char *resp,
                                            int cap);
   int openai_format_responses_fc_args_delta(const char *item_id, int output_index,
                                             const char *delta, char *resp, int cap);
   int openai_format_responses_fc_args_done(const char *item_id, int output_index,
                                            const char *arguments, char *resp, int cap);
   int openai_format_responses_completed_items(const char *id, const char *model, long created,
                                               struct cJSON *output_arr, int prompt_tokens,
                                               int completion_tokens, int cached_tokens, char *resp,
                                               int cap);

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
   /* As openai_format_chat_completion with an explicit finish_reason ("stop",
    * "length", "content_filter"); NULL means "stop". */
   int openai_format_chat_completion_reason(const char *id, const char *model, const char *content,
                                            long created, int prompt_tokens, int completion_tokens,
                                            const char *finish_reason, char *resp, int cap);

   /* Build a legacy text_completion object into resp[cap]:
    * {"id":id,"object":"text_completion","created":created,"model":model,
    *  "choices":[{"text":content,"index":0,"finish_reason":"stop"}],
    *  "usage":{"prompt_tokens":pt,"completion_tokens":ct,"total_tokens":pt+ct}}
    * Returns bytes written, or -1. */
   /* Omit optional usage when its totals were not observed. */
   int openai_format_text_completion(const char *id, const char *model, const char *content,
                                     long created, int prompt_tokens, int completion_tokens,
                                     int usage_unobserved, char *resp, int cap);

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

#ifdef __cplusplus
}
#endif

#endif /* DEC_OPENAI_SHAPE_H */
