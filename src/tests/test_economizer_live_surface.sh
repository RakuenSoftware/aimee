#!/bin/sh
set -eu

fail()
{
    echo "economizer live-surface: $1" >&2
    exit 1
}

if grep -R -n 'aimee_backend_anthropic_set_cache_enabled' headers server posix >/dev/null; then
    fail "economizer still controls Anthropic cache decoration"
fi

for source in posix/agent_runtime.c server/openai_chat.c server/anthropic_http.c; do
    grep -q 'wire_fence_select' "$source" || fail "$source bypasses the wire snapshot selector"
done

# SAFE has exactly one live transform: strict JSON compaction at the fresh local
# tool-result boundary. Lossy history mutation must remain behind AGGRESSIVE.
grep -q 'agent_economize_fresh_tool_result(result_str)' posix/agent_runtime.c ||
    fail "fresh tool output does not pass through the safe compactor"
grep -q 'preset.json_compact' posix/agent_runtime.c ||
    fail "safe JSON compaction is not controlled by its preset"
grep -q '!anthropic && (preset.history_fold || preset.compress)' posix/agent_runtime.c ||
    fail "agent history reduction is not gated to aggressive OpenAI-family routes"
# The chatgpt route folds. It was excluded for years as "unverified", which made
# the fold unreachable on exactly the route a codex-provider deployment uses --
# measured as a 0-token difference on CT 403. Anthropic stays excluded above (its
# exact prefix controls cache reuse); chatgpt must not be re-added beside it.
grep -q 'mreq.history_fold = preset.history_fold || config_fold_enabled();' posix/agent_runtime.c ||
    fail "the delegate history fold is gated again (chatgpt exclusion reintroduced?)"
grep -q 'gw_mutate_upstream_ok(parity)' server/anthropic_http.c ||
    fail "Anthropic ingress mutation does not preserve native Anthropic prefixes"
grep -q 'gw_mutate_upstream_ok(upstream_is_anthropic)' server/openai_chat.c ||
    fail "OpenAI ingress mutation does not exclude Anthropic upstreams"

# The economizer never adds, removes, or moves provider cache controls and does
# not perform a remote token-counting preflight.
if grep -n -E 'cache_control|prompt_cache_key|count_tokens' \
    modules/economizer/gateway_mutate.c modules/economizer/gateway_mutate_wire.c \
    posix/agent_runtime.c >/dev/null; then
    fail "economizer controls provider caching or remote token counting"
fi

grep -q 'http_retry_post_context_bytes' posix/agent_runtime.c ||
    fail "delegate retries do not use the exact-length snapshot transport"
grep -q 'http_retry_post_context_bytes' server/openai_chat.c ||
    fail "OpenAI ingress does not use the exact-length snapshot transport"
grep -q 'agent_http_post_bytes' server/anthropic_http.c ||
    fail "Anthropic buffered ingress does not use the exact-length snapshot transport"
grep -q 'agent_http_post_stream_bytes' server/anthropic_http.c ||
    fail "Anthropic streaming ingress does not use the exact-length snapshot transport"

echo "economizer live-surface: ok"
