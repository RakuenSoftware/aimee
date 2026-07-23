#!/bin/sh
set -eu

fail()
{
    echo "economizer live-surface: $1" >&2
    exit 1
}

live_sources="posix server"
legacy_calls='context_reduce[[:space:]]*\(|tool_condense_apply[[:space:]]*\(|build_fold_view[[:space:]]*\(|agent_compress_tool_result[[:space:]]*\('
if grep -R -n -E "$legacy_calls" $live_sources |
    grep -v '^server/agent_policy.c:[0-9][0-9]*:char \*agent_compress_tool_result' >/dev/null; then
    grep -R -n -E "$legacy_calls" $live_sources |
        grep -v '^server/agent_policy.c:[0-9][0-9]*:char \*agent_compress_tool_result' >&2
    fail "legacy reducer remains reachable from a production request source"
fi

if grep -n 'modules/economizer/gateway_mutate_wire.c' Makefile >/dev/null; then
    fail "legacy gateway mutation remains linked into production"
fi

if grep -R -n 'aimee_backend_anthropic_set_cache_enabled' headers server posix >/dev/null; then
    fail "economizer still controls Anthropic cache decoration"
fi

for source in posix/agent_runtime.c server/openai_chat.c server/anthropic_http.c; do
    grep -q 'econ_wire_select' "$source" || fail "$source bypasses the wire snapshot selector"
done

# Activation machinery may ship, but the production registry is still empty.
# No live request path may construct or consume a candidate before a separately
# reviewed provider tuple is signed.
if grep -R -n -E 'econ_json_compact[[:space:]]*\(|econ_provenance_issue_local[[:space:]]*\(|econ_dispatch_lease_begin[[:space:]]*\(' $live_sources >/dev/null; then
    grep -R -n -E 'econ_json_compact[[:space:]]*\(|econ_provenance_issue_local[[:space:]]*\(|econ_dispatch_lease_begin[[:space:]]*\(' $live_sources >&2
    fail "dormant activation machinery is reachable from a production request source"
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
