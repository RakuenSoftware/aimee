# Proposal: Claude Code Anthropic ingress (`/v1/messages`)

- **State:** done
- **Status refreshed:** 2026-06-09
- **Moved from:** `docs/proposals/pending/claude-code-anthropic-ingress.md`

## Shipped

This proposal is complete.

- `POST /v1/messages` is implemented for buffered Anthropic Messages requests.
- `POST /v1/messages` with `stream:true` is implemented as Anthropic typed-event SSE.
- `POST /v1/messages/count_tokens` returns the estimated input token count.
- Anthropic `x-api-key` authentication is supported for Anthropic-profile agents.
- `aimee claude-proxy enable|disable` updates Claude Code integration settings.
- The ingress remains a stateless wire-format proxy by design; memory/preinject behavior is handled outside the `/v1/messages` wire.

## Verification Notes

Verified in-tree evidence: `src/server/anthropic_ingress.c`, `src/server/anthropic_http.c`, `src/server/server_http_routes.inc`, `src/client_integrations.c`, `src/cli_main.c`, `src/tests/test_anthropic_ingress.c`, and `src/tests/test_server_http.c`.
