/* cli_mistral.c: Mistral native adapter for legacy provider-CLI routes. */
#include "provider_cli_adapter.h"

const provider_cli_adapter_t mistral_provider_cli_adapter = {
    .cli_kind = "mistral",
    .display_name = "Mistral native adapter",
    .caps = {.max_context_tokens = 0,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_NATIVE,
             .write_confidence = 0.9f},
    .native_provider = "mistral",
    .native_default_endpoint = "https://api.mistral.ai/v1",
    .native_default_model = "mistral-vibe-cli-latest",
    .native_api_key_env = "MISTRAL_API_KEY",
};
