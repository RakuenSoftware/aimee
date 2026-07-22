/* provider_client.h: shared LLM-provider client (DRY across aimee-server + aimee-kb).
 *
 * Given a provider def (base_url + model + key + wire format) and a request
 * (messages + optional JSON schema), return a completion — owning the wire
 * format, transport, and retries in one place. Both aimee-server (per-user
 * provider defs, client-held credentials) and aimee-kb (operator-owned curator
 * provider) link this. This is §1 of the curator-llm-backend proposal: the
 * foundation §2 (curator stages -> providers) and §4 (health) build on.
 *
 * Transport is agent_http_post (the platform object both binaries already link),
 * and the module owns its own small retry loop so aimee-kb does not need the
 * server-only http_retry/failover modules (which are DB1-coupled).
 *
 * Pure domain API: no agent_t, no server config, no provider-registry types in
 * any signature — only a plain provider def the caller fills from its own config. */
#ifndef DEC_PROVIDER_CLIENT_H
#define DEC_PROVIDER_CLIENT_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   struct cJSON;

/* Buffer for a provider-reported model id. Kept local so this header stays
 * self-contained (matches MAX_MODEL_LEN in agent_types.h). */
#define PROVIDER_MODEL_LEN 128

   typedef enum
   {
      PROVIDER_WIRE_OPENAI_CHAT = 0, /* OpenAI-compatible /chat/completions */
   } provider_wire_t;

   typedef struct
   {
      const char *base_url; /* API base, e.g. "http://host:8080/v1"; the
                             * "/chat/completions" suffix is appended and a
                             * trailing slash is tolerated. */
      const char *model;    /* model id sent in the request body. */
      const char *api_key;  /* bearer token; NULL/"" for a keyless local endpoint. */
      provider_wire_t wire; /* wire format. */
      int timeout_ms;       /* per-attempt timeout; <=0 => default. */
      double temperature;   /* <0 => omit (let the provider default). */
      int max_tokens;       /* <=0 => omit. */
      int max_attempts;     /* total attempts incl. the first; <=0 => default. */
      int disable_thinking; /* !=0 => send chat_template_kwargs.enable_thinking=false
                             * so a reasoning model skips its chain-of-thought pass.
                             * For mechanical, high-volume stages (Tier-A extract/
                             * index) the reasoning pass only adds latency and can
                             * truncate the answer; the flag makes output compact and
                             * deterministic. Ignored by endpoints that don't support
                             * the jinja chat-template kwarg. */
   } provider_def_t;

   typedef struct
   {
      char *content;                  /* malloc'd completion text; NULL if none. Caller frees. */
      char *reasoning;                /* malloc'd reasoning/chain-of-thought, when the provider
                                       * returned any: either as `reasoning_content` (llama.cpp
                                       * --jinja, DeepSeek/Qwen APIs) or as a <think> preamble on
                                       * an inlining endpoint. Kept SEPARATE from content rather
                                       * than discarded — dropping it silently is what left the
                                       * curator unable to say why an extraction went wrong, and
                                       * the canonical IR models it as AIMEE_BLK_THINKING. NULL
                                       * when the provider sent none. Caller frees. */
      int prompt_tokens;              /* from usage; 0 when absent. */
      int completion_tokens;          /* from usage; 0 when absent. */
      char model[PROVIDER_MODEL_LEN]; /* provider-reported model id; "" when absent. */
      char finish_reason[32];         /* OpenAI finish_reason; "" when absent. */
      int http_status;                /* last HTTP status (or <0 on a network error). */
   } provider_completion_t;

#define PROVIDER_CLIENT_DEFAULT_TIMEOUT_MS 120000
#define PROVIDER_CLIENT_DEFAULT_ATTEMPTS   3

   /* Build the OpenAI chat-completions request body for `def` + `messages` (a
    * cJSON array of {role, content}). When `json_schema` is non-NULL it is sent as
    * response_format:{type:"json_schema", json_schema:{name, schema, strict:true}}
    * so a grammar-capable endpoint (llama.cpp --jinja, OpenAI) returns constrained
    * JSON. `messages`/`json_schema` are NOT taken over — the result references deep
    * copies. Returns a new cJSON object (caller cJSON_Delete) or NULL on failure. */
   struct cJSON *provider_client_build_openai(const provider_def_t *def, struct cJSON *messages,
                                              struct cJSON *json_schema);

   /* Parse an OpenAI chat-completions response `body` into `out` (zeroed first).
    * Returns 0 when choices[0].message.content is present, -1 otherwise. Usage,
    * model and finish_reason are filled when present. `http_status` is left 0
    * here — provider_client_complete sets it after the transport call. */
   int provider_client_parse_openai(const char *body, provider_completion_t *out);

   /* Build + POST (with retry) + parse. Returns 0 on success; on failure returns
    * -1 and writes a message into errbuf (when errlen > 0). `out` is zeroed on
    * entry and is always safe to pass to provider_completion_free afterward. */
   int provider_client_complete(const provider_def_t *def, struct cJSON *messages,
                                struct cJSON *json_schema, provider_completion_t *out, char *errbuf,
                                size_t errlen);

   /* Free heap-owned fields of `out` (idempotent; does not free `out` itself). */
   void provider_completion_free(provider_completion_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PROVIDER_CLIENT_H */
