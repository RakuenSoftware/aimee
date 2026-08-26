# IR golden-test fixture corpus

Inputs for the canonical-IR golden tests. Each fixture is a real-shaped client
request. The Slice-1 golden harness will, per fixture:

1. **frontend.parse** the request → `aimee_request_t`, and assert the IR captured
   the documented semantics (no dropped blocks, tool ids/order preserved,
   cache_control kept).
2. **Same-protocol round-trip parity**: `frontend.render`/`backend.build` back to
   the SAME wire → assert byte-parity (or documented semantic parity) with the
   original. This is the prompt-cache invariant.
3. **Cross-protocol semantic equivalence**: fixtures tagged with a `pair_id` are the
   SAME semantic turn expressed in two wires; parse both → assert
   `aimee_ir_request_equal` (identical IR → identical KB input + identical backend
   build). This is the regression that proves "codex primary breaks Claude Code" and
   "KB gets messages incorrectly" are closed.

## Required coverage matrix (the hard cases the roundtable flagged)

| case                                   | why it bites                                  |
|----------------------------------------|-----------------------------------------------|
| basic single user text turn            | baseline parse/render/round-trip              |
| **parallel** tool_use + tool_result    | mismatched ids route outputs to the wrong tool (also injection) |
| tool_use args with unicode/nested json | opaque args must survive byte-for-byte        |
| per-block `cache_control` (Anthropic)  | dropping it destroys prompt-cache economics   |
| thinking/reasoning blocks              | Claude Code + o-series depend on it           |
| system as an ARRAY of blocks           | flattening to a string loses cache/structure  |
| image/document blocks                  | silently dropped by today's text extractor    |
| stop_sequences                         | Anthropic-only; missing = behavior change     |
| multi-turn with interleaved tool calls | ordering + role alternation                   |

## Naming
- `anthropic_<case>.json`, `openai_chat_<case>.json`, `responses_<case>.json`
- cross-protocol pairs share a `"_pair_id"` top-level key (stripped before parse).

## Provenance / privacy
Fixtures are HAND-AUTHORED synthetic payloads, never captured raw user/production
traffic (raw bodies must not enter fixtures, logs, or KB per the security ruling).
A live capture harness (for the byte-parity fixture) must scrub content first.
