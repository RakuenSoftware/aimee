# Proposal: Codex frontend ingress (`/v1/responses`)

- **State:** done
- **Status refreshed:** 2026-06-09
- **Moved from:** `docs/proposals/pending/codex-frontend-ingress.md`

## Shipped

This proposal is complete.

- `POST /v1/responses` is wired through `server_http.c` and `openai_chat.c`, including streaming SSE.
- Codex-compatible Responses API shapes are implemented in `openai_shape.c`.
- The Codex tool loop is supported: incoming `function_call` and `function_call_output` history is translated to provider chat messages, and outbound tool calls are emitted as Responses API `function_call` output items.
- `/v1/models` remains OpenAI/Codex shaped.
- `SHTTP_MAX_BODY` was raised to 4 MB so Codex's large structured tool payloads are accepted.
- Unit coverage exists for the Responses store, OpenAI shape parsing/formatting, server HTTP route wiring, and Responses parser behavior.

## Verification Notes

Verified in-tree evidence: `src/server/server_http.c`, `src/server/openai_chat.c`, `src/server/openai_shape.c`, `src/tests/test_openai_shape.c`, `src/tests/test_openai_responses_store.c`, and `src/tests/test_server_http.c`.
