/* delegate_openai.c: OpenAI-compatible provider driver (covers openai, ollama, chatgpt) */
#include "aimee.h"
#include "delegate_driver.h"
#include "agent_protocol.h"
#include "agent_tools.h"
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak, noinline)) int payload_rewrite_track_request(const char *system_prompt,
                                                                  const char *static_context,
                                                                  int current_tokens,
                                                                  int model_context_limit)
#else
static int payload_rewrite_track_request(const char *system_prompt, const char *static_context,
                                         int current_tokens, int model_context_limit)
#endif
{
   (void)system_prompt;
   (void)static_context;
   (void)current_tokens;
   (void)model_context_limit;
   return 0;
}

/* --- OpenAI chat/completions driver --- */

static int url_has_suffix(const char *s, const char *suffix)
{
   size_t s_len, suffix_len;
   if (!s || !suffix)
      return 0;
   s_len = strlen(s);
   suffix_len = strlen(suffix);
   if (s_len < suffix_len)
      return 0;
   return strcmp(s + s_len - suffix_len, suffix) == 0;
}

static int endpoint_is_host_root(const char *endpoint)
{
   const char *p = strstr(endpoint, "://");
   if (p)
      p += 3;
   else
      p = endpoint;
   return strchr(p, '/') == NULL;
}

static void trim_trailing_slashes(const char *src, char *dst, size_t dst_len)
{
   size_t len;
   if (!dst_len)
      return;
   snprintf(dst, dst_len, "%s", src ? src : "");
   len = strlen(dst);
   while (len > 0 && dst[len - 1] == '/')
      dst[--len] = '\0';
}

static int openai_join_endpoint(const char *endpoint, const char *suffix, char *url, size_t url_len)
{
   char base[MAX_ENDPOINT_LEN];

   trim_trailing_slashes(endpoint, base, sizeof(base));
   if (!base[0])
      return -1;
   if (url_has_suffix(base, suffix))
   {
      snprintf(url, url_len, "%s", base);
      return 0;
   }
   if (endpoint_is_host_root(base))
   {
      snprintf(url, url_len, "%s/v1%s", base, suffix);
      return 0;
   }
   snprintf(url, url_len, "%s%s", base, suffix);
   return 0;
}

static int openai_build_url(const agent_t *agent, char *url, size_t url_len)
{
   return openai_join_endpoint(agent->endpoint, "/chat/completions", url, url_len);
}

static cJSON *openai_build_request(const agent_t *agent, cJSON *messages, cJSON *tools,
                                   const char *system_prompt, int max_tokens, double temperature)
{
   /* System prompt goes in the messages array for OpenAI (already handled by caller) */
   (void)system_prompt;
   return agent_build_request_openai(agent, messages, tools, max_tokens, temperature);
}

static void openai_parse_response(cJSON *root, const char *body, parsed_response_t *out)
{
   (void)body;
   agent_parse_response_openai(root, out);
}

static cJSON *openai_build_tools(void)
{
   return build_tools_array();
}

static void openai_get_caps(const agent_t *agent, driver_caps_t *caps)
{
   caps->capability_flags = DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING | DRIVER_CAP_VISION;
   caps->max_output_tokens = 0;

   /* Heuristic context limits based on model name */
   const char *m = agent->model;
   if (strstr(m, "gpt-4o") || strstr(m, "gpt-4-turbo"))
      caps->context_limit = DRIVER_CTX_HUGE;
   else if (strstr(m, "gpt-4"))
      caps->context_limit = DRIVER_CTX_LARGE;
   else if (strstr(m, "gpt-3.5"))
      caps->context_limit = DRIVER_CTX_MEDIUM;
   else
      caps->context_limit = DRIVER_CTX_LARGE;
}

const delegate_driver_t delegate_driver_openai = {
    .name = "openai",
    .build_url = openai_build_url,
    .build_request = openai_build_request,
    .parse_response = openai_parse_response,
    .build_tools = openai_build_tools,
    .get_caps = openai_get_caps,
};

/* --- ChatGPT Responses API driver --- */

static int chatgpt_build_url(const agent_t *agent, char *url, size_t url_len)
{
   return openai_join_endpoint(agent->endpoint, "/responses", url, url_len);
}

static cJSON *chatgpt_build_request(const agent_t *agent, cJSON *messages, cJSON *tools,
                                    const char *system_prompt, int max_tokens, double temperature)
{
   (void)max_tokens;
   (void)temperature;
   return agent_build_request_responses(agent, messages, tools, system_prompt);
}

static void chatgpt_parse_response(cJSON *root, const char *body, parsed_response_t *out)
{
   agent_parse_response_responses(body, out);
   (void)root;
}

static cJSON *chatgpt_build_tools(void)
{
   return build_tools_array_responses();
}

static void chatgpt_get_caps(const agent_t *agent, driver_caps_t *caps)
{
   (void)agent;
   caps->capability_flags =
       DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING | DRIVER_CAP_VISION | DRIVER_CAP_SYSTEM_MSG;
   caps->context_limit = DRIVER_CTX_HUGE;
   caps->max_output_tokens = 0;
}

const delegate_driver_t delegate_driver_chatgpt = {
    .name = "chatgpt",
    .build_url = chatgpt_build_url,
    .build_request = chatgpt_build_request,
    .parse_response = chatgpt_parse_response,
    .build_tools = chatgpt_build_tools,
    .get_caps = chatgpt_get_caps,
};

/* --- Anthropic Messages API driver --- */

static void anthropic_get_caps(const agent_t *agent, driver_caps_t *caps);

static int delegate_prompt_token_estimate(cJSON *messages, const char *system_prompt)
{
   int tokens = 0;
   char *serialised = messages ? cJSON_PrintUnformatted(messages) : NULL;

   if (serialised)
   {
      tokens += (int)(strlen(serialised) / 4) + 1;
      free(serialised);
   }
   if (system_prompt && system_prompt[0])
      tokens += (int)(strlen(system_prompt) / 4) + 1;
   return tokens;
}

static void delegate_payload_rewrite_track(cJSON *messages, const char *system_prompt,
                                           int model_context_limit)
{
   payload_rewrite_track_request(system_prompt, NULL,
                                 delegate_prompt_token_estimate(messages, system_prompt),
                                 model_context_limit);
}

static int anthropic_build_url(const agent_t *agent, char *url, size_t url_len)
{
   /* The Anthropic Messages API always lives at <base>/v1/messages. Unlike the
    * OpenAI join, the /v1 version segment is part of the operation path rather
    * than the caller-supplied base, so we insert it for ANY base that does not
    * already carry it — not only bare host roots (which is all
    * openai_join_endpoint would handle). This lets anthropic-compatible
    * gateways be registered with a path-prefixed base (e.g.
    * https://api.minimax.io/anthropic) without hand-baking /v1 into the
    * endpoint; without it the join produced .../anthropic/messages -> 404. */
   char base[MAX_ENDPOINT_LEN];

   trim_trailing_slashes(agent->endpoint, base, sizeof(base));
   if (!base[0])
      return -1;
   if (url_has_suffix(base, "/messages")) /* already a full messages URL */
      snprintf(url, url_len, "%s", base);
   else if (url_has_suffix(base, "/v1")) /* base already carries the version */
      snprintf(url, url_len, "%s/messages", base);
   else
      snprintf(url, url_len, "%s/v1/messages", base);
   return 0;
}

static cJSON *anthropic_build_request(const agent_t *agent, cJSON *messages, cJSON *tools,
                                      const char *system_prompt, int max_tokens, double temperature)
{
   driver_caps_t caps;
   anthropic_get_caps(agent, &caps);
   delegate_payload_rewrite_track(messages, system_prompt, caps.context_limit);
   return agent_build_request_anthropic(agent, messages, tools, system_prompt, max_tokens,
                                        temperature);
}

static void anthropic_parse_response(cJSON *root, const char *body, parsed_response_t *out)
{
   (void)body;
   agent_parse_response_anthropic(root, out);
}

static cJSON *anthropic_build_tools(void)
{
   return build_tools_array_anthropic();
}

static void anthropic_get_caps(const agent_t *agent, driver_caps_t *caps)
{
   caps->capability_flags =
       DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING | DRIVER_CAP_VISION | DRIVER_CAP_SYSTEM_MSG;
   caps->max_output_tokens = 0;

   const char *m = agent->model;
   if (strstr(m, "claude-opus-4") || strstr(m, "claude-3-5"))
      caps->context_limit = DRIVER_CTX_HUGE;
   else if (strstr(m, "claude-3"))
      caps->context_limit = DRIVER_CTX_LARGE;
   else
      caps->context_limit = DRIVER_CTX_LARGE;
}

const delegate_driver_t delegate_driver_anthropic = {
    .name = "anthropic",
    .build_url = anthropic_build_url,
    .build_request = anthropic_build_request,
    .parse_response = anthropic_parse_response,
    .build_tools = anthropic_build_tools,
    .get_caps = anthropic_get_caps,
};

/* --- OpenRouter driver (OpenAI-compatible routing layer) --- */

static void openrouter_get_caps(const agent_t *agent, driver_caps_t *caps)
{
   caps->capability_flags = DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING | DRIVER_CAP_VISION;
   caps->max_output_tokens = 0;

   /* Model IDs on OpenRouter are "provider/name" — match on well-known context tiers. */
   const char *m = agent->model;
   if (strstr(m, "gemini-2") || strstr(m, "gemini-1.5") || strstr(m, "claude-3-5") ||
       strstr(m, "claude-opus-4") || strstr(m, "claude-sonnet-4") || strstr(m, "gpt-4o"))
      caps->context_limit = DRIVER_CTX_HUGE;
   else
      caps->context_limit = DRIVER_CTX_LARGE;
}

const delegate_driver_t delegate_driver_openrouter = {
    .name = "openrouter",
    .build_url = openai_build_url,
    .build_request = openai_build_request,
    .parse_response = openai_parse_response,
    .build_tools = openai_build_tools,
    .get_caps = openrouter_get_caps,
};

/* --- Mistral driver (OpenAI-compatible chat/completions) --- */

static void mistral_get_caps(const agent_t *agent, driver_caps_t *caps)
{
   (void)agent;
   caps->capability_flags = DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING;
   caps->context_limit = DRIVER_CTX_LARGE;
   caps->max_output_tokens = 0;
}

const delegate_driver_t delegate_driver_mistral = {
    .name = "mistral",
    .build_url = openai_build_url,
    .build_request = openai_build_request,
    .parse_response = openai_parse_response,
    .build_tools = openai_build_tools,
    .get_caps = mistral_get_caps,
};

/* --- MiniMax driver (OpenAI-compatible chat/completions) --- */

static void minimax_get_caps(const agent_t *agent, driver_caps_t *caps)
{
   caps->capability_flags = DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING;
   /* MiniMax-M2.7 advertises 200k context for function-calling sessions. */
   if (agent && strstr(agent->model, "M2.7"))
      caps->context_limit = 200000;
   else
      caps->context_limit = DRIVER_CTX_LARGE;
   caps->max_output_tokens = 0;
}

const delegate_driver_t delegate_driver_minimax = {
    .name = "minimax",
    .build_url = openai_build_url,
    .build_request = openai_build_request,
    .parse_response = openai_parse_response,
    .build_tools = openai_build_tools,
    .get_caps = minimax_get_caps,
};
