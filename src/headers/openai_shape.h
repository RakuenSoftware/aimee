/* openai_shape.h: OpenAI-compatible JSON shaping helpers (no sockets, no network). */
#ifndef DEC_OPENAI_SHAPE_H
#define DEC_OPENAI_SHAPE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Bounded provider diagnostic. Only recognized JSON error text is exposed;
    * never copy a raw HTML/proxy response or unrelated response fields. */
   void openai_upstream_error_message(int status, const char *body, char *out, size_t cap);

   /* Build the terminal chat.completion.chunk with an empty delta and an
    * explicit finish_reason ("stop", "tool_calls", …). openai_format_chat_chunk's
    * finish frame always says "stop"; a tool-call turn must not. Returns bytes
    * written (excluding NUL), or -1 if it does not fit. */
   int openai_format_chat_chunk_finish(const char *id, const char *model, long created,
                                       const char *reason, char *resp, int cap);

   /* Initial Responses SSE envelope; successful output is rendered from IR. */
   int openai_format_responses_created(const char *id, const char *model, long created, char *resp,
                                       int cap);

   /* Terminal Responses error envelope; returns bytes written or -1. */
   int openai_format_responses_failed(const char *id, const char *model, long created,
                                      const char *code, const char *message, char *resp, int cap);

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
