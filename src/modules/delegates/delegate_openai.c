/* delegate_openai.c: OpenAI-compatible provider driver (covers openai, ollama, chatgpt) */
#include "aimee.h"
#include <aimee/delegates/delegate_driver.h>
#include <string.h>

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
    .get_caps = openai_get_caps,
};

/* --- ChatGPT Responses API driver --- */

static int chatgpt_build_url(const agent_t *agent, char *url, size_t url_len)
{
   return openai_join_endpoint(agent->endpoint, "/responses", url, url_len);
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
    .get_caps = chatgpt_get_caps,
};

/* --- Anthropic Messages API driver --- */

static void anthropic_get_caps(const agent_t *agent, driver_caps_t *caps);

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
    .get_caps = minimax_get_caps,
};
