# Core modularization slice 32: complete translation ownership

## Scope

This slice marks the required `translation` descriptor `ownership_complete: true`. The existing
descriptor already owns all eight module-local C sources, three canonical public headers, five direct
tests, and `docs/modules/translation.md`. The change is metadata, validation, documentation, and cleanup
accounting only: production source, public symbols, build inputs, runtime behavior, and wire behavior do
not change.

Translation depends on the ownership-complete `ir` module and `module-runtime`; the already-complete
`protocols` module depends on translation. Completing translation independently therefore closes the
declared IR-to-protocol ownership chain in graph order.

## Source liveness and ownership

All eight sources are shipping inputs in `src/Makefile:440`, and every source contains at least one
production-selected path:

- `aimee_frontend_anthropic.c`: `anthropic_frontend_parse` is called by `server/aimee_ir_serve.c` and
  `server/aimee_ir_shadow.c`.
- `aimee_frontend_openai.c`: `openai_frontend_parse` is called by `server/aimee_ir_serve.c`.
- `aimee_frontend_responses.c`: `responses_frontend_parse` is called by `server/aimee_ir_serve.c`.
- `aimee_backend_anthropic.c`: build, parse, and cache-control entrypoints are called by
  `server/agent_request_build.c`, `server/server.c`, POSIX agent parsing/runtime, and
  `server/aimee_ir_shadow.c`.
- `aimee_backend_openai.c`: build and parse entrypoints are called by request building, IR serve/shadow,
  POSIX agent parsing/runtime, and `server/anthropic_http.c`.
- `aimee_backend_responses.c`: build and parse entrypoints are called by request building, IR serve,
  POSIX agent parsing, `server/agent_bridge.c`, and IR shadow.
- `aimee_backend_bedrock.c`: `bedrock_converse_build` is called by `kb/kb_bedrock_egress.c`, and
  `converse_stop_reason` is shared with the stream implementation.
- `aimee_ir_stream.c`: OpenAI stream parsing and split Anthropic emission are called by
  `server/aimee_ir_serve.c` and `server/anthropic_http.c`.

These files translate only between provider/client wire shapes and provider-neutral IR. Their public
headers are consumed by the production callers above, so `translation` is the correct owner. The five
descriptor-owned tests cover frontend parsing/rendering, backend building/parsing, Bedrock Converse,
OpenAI/Anthropic streaming, and Bedrock ConverseStream. Adjacent server/parity tests remain consumer-owned.

## DRY and dead-code findings

No complete source file is self-tested-only, and no duplicate canonical adapter implementation was found.
Exact symbol searches in the merged feature-branch worktree found only test callers for:

- `anthropic_frontend_render`, `openai_frontend_render`, and `responses_frontend_render`;
- `bedrock_converse_parse`;
- `converse_stream_state_init` and `bedrock_converse_stream_to_deltas`; and
- `anthropic_delta_render`.

These are explicit API cleanup or activation candidates, not evidence that their containing source is
dead. They model intended buffered egress or Bedrock streaming behavior and require separate compatibility
and readiness decisions before deletion, privatization, or activation.

The first decision roundtable cited `src/kb/http/kb_http_egress.c:321` and lines 1200-1481 of
`src/kb/kb_bedrock_egress.c` as production callers. Those references are stale: the first path is absent
from the merged worktree, the second file has 111 lines, and exact searches under `src/kb` find none of
the four cited symbols. Current-worktree evidence therefore controls this ownership audit.

Legacy serve/shadow and direct wire translators remain outside module-local ownership pending their own
parity-backed behavior-separation slices. Their existence does not create a second canonical adapter API.

## Regression controls

Production descriptor mutations remove one frontend source, one backend source, and the stream source;
plant an undeclared source and private header; and remove the canonical translation document. Each must
fail with the stable `ownership-complete` rule. The existing header-layout and source-ownership gates
continue to reject retired flat paths, basename includes, and broad source include roots.

## Verification

- descriptor validation and its complete production mutation suite;
- module header-layout, source-ownership, docs, cleanup-ledger, and refactor-baseline checks plus focused
  mutation suites;
- full Make build and lint;
- build and execute `unit-test-aimee-frontend`, `unit-test-aimee-backend`,
  `unit-test-aimee-backend-bedrock`, `unit-test-aimee-ir-stream`, and
  `unit-test-aimee-converse-stream`; and
- technical-writer review, exact-final-diff roundtable approval, and all required pull-request checks.
