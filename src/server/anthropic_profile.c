/* anthropic_profile.c: model_provider_t profile for Anthropic. */
#include "model_provider.h"
#include "aimee.h"
#include <stdlib.h>
#include <string.h>

static const char *anthropic_env_vars[] = {"ANTHROPIC_API_KEY", NULL};
static const char *anthropic_default_headers[] = {"anthropic-version", "2023-06-01", NULL};

static const char *anthropic_routable_models[] = {"claude-opus-4-8", "claude-sonnet-5", "claude-haiku-4-5", NULL};

model_provider_t anthropic_provider = {
    .name = "anthropic",
    .display_name = "Anthropic",
    .description = "Anthropic Claude API (claude-sonnet, claude-opus, claude-haiku)",
    .base_url = "https://api.anthropic.com/v1",
    .models_url = NULL,
    .signup_url = "https://console.anthropic.com/account/keys",
    .auth_type = "x-api-key",
    .env_vars = anthropic_env_vars,
    .api_mode = API_MODE_ANTHROPIC_MESSAGES,
    .default_model = "claude-sonnet-4-6",
    .default_aux_model = "claude-haiku-4-5-20251001",
    .fallback_models = NULL,
    .routable_models = anthropic_routable_models,
    .fixed_temperature = -1,
    .default_max_tokens = 8192,
    .default_headers = anthropic_default_headers,
    .fetch_models = NULL,
};
