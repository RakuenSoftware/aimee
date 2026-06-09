/* delegate_driver.h: provider driver interface for delegate dispatch */
#ifndef DEC_DELEGATE_DRIVER_H
#define DEC_DELEGATE_DRIVER_H 1

#include "agent_types.h"
#include "agent_protocol.h"

struct cJSON;

/* Maximum URL length when a driver builds an endpoint URL */
#define DRIVER_URL_MAX 1024

/* Capability flags */
#define DRIVER_CAP_TOOL_CALLS (1 << 0) /* native JSON tool calling */
#define DRIVER_CAP_STREAMING  (1 << 1) /* streaming responses */
#define DRIVER_CAP_VISION     (1 << 2) /* image inputs */
#define DRIVER_CAP_SYSTEM_MSG (1 << 3) /* system message in request body (vs. messages array) */
#define DRIVER_CAP_PROMPT_CACHE                                                                    \
   (1 << 4) /* provider-side prompt caching (e.g. Gemini cachedContent) */

/* Context window sizes (in tokens) for capability metadata */
#define DRIVER_CTX_SMALL  4096
#define DRIVER_CTX_MEDIUM 16384
#define DRIVER_CTX_LARGE  128000
#define DRIVER_CTX_HUGE   200000

typedef struct
{
   int capability_flags;  /* DRIVER_CAP_* bitmask */
   int context_limit;     /* max input context tokens */
   int max_output_tokens; /* max generated tokens per call (0 = driver default) */
} driver_caps_t;

/*
 * delegate_driver_t: vtable interface for a provider driver.
 *
 * Each driver implements:
 *   build_url        — construct the endpoint URL from the agent config
 *   build_request    — produce the JSON request body
 *   parse_response   — parse a JSON response body into parsed_response_t
 *   build_tools      — return a provider-specific tools JSON array (caller frees)
 *   get_caps         — fill capability metadata for this agent/model
 *
 * All functions may be NULL; if so, a fallback behaviour applies:
 *   build_url   NULL → use agent->endpoint + "/chat/completions"
 *   build_tools NULL → use build_tools_array() (OpenAI format)
 *   get_caps    NULL → DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING, DRIVER_CTX_LARGE
 */
typedef struct delegate_driver
{
   const char *name; /* provider identifier, e.g. "openai", "anthropic", "gemini" */

   /* Build the full request URL into url (max DRIVER_URL_MAX bytes).
    * Returns 0 on success, -1 on error. */
   int (*build_url)(const agent_t *agent, char *url, size_t url_len);

   /* Build the request JSON body.
    * messages: conversation history (caller owns)
    * tools:    tools array from build_tools() (caller owns)
    * Returns a new cJSON object (caller must cJSON_Delete). */
   struct cJSON *(*build_request)(const agent_t *agent, struct cJSON *messages, struct cJSON *tools,
                                  const char *system_prompt, int max_tokens, double temperature);

   /* Parse the response JSON (root) into out.
    * body is the raw response string (may be needed for streaming formats). */
   void (*parse_response)(struct cJSON *root, const char *body, parsed_response_t *out);

   /* Return a new tools JSON array in provider format (caller must cJSON_Delete). */
   struct cJSON *(*build_tools)(void);

   /* Fill capability metadata for agent/model. */
   void (*get_caps)(const agent_t *agent, driver_caps_t *caps);
} delegate_driver_t;

/* --- Driver registry --- */

/* Register a driver. Must be called before delegate_driver_get(). */
void delegate_driver_register(const delegate_driver_t *driver);

/* Look up a driver by provider name. Returns NULL if not found.
 * Falls back to the "openai" driver for unknown providers. */
const delegate_driver_t *delegate_driver_get(const char *provider);

/* Register all built-in drivers (openai, anthropic, chatgpt, gemini).
 * Called once at startup. Safe to call multiple times (idempotent). */
void delegate_drivers_init(void);

/* --- Convenience helpers --- */

/* Build URL using driver (or OpenAI default if driver->build_url is NULL). */
int delegate_build_url(const delegate_driver_t *driver, const agent_t *agent, char *url,
                       size_t url_len);

/* Get capabilities, filling defaults if driver->get_caps is NULL. */
void delegate_get_caps(const delegate_driver_t *driver, const agent_t *agent, driver_caps_t *caps);

#endif /* DEC_DELEGATE_DRIVER_H */
