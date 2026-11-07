/* Claude registration metadata. Execution is owned by the native factory. */
#include "provider_cli_adapter.h"

const provider_cli_adapter_t claude_provider_cli_adapter = {
    .cli_kind = "claude",
    .display_name = "Claude CLI",
    .caps = {.max_context_tokens = 200000,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_STABLE,
             .write_confidence = 0.95f},
};
