# E4 task-conditioned code context validation

- **Date:** 2026-07-30 UTC
- **Slice:** E4 of `agent-facing-code-intelligence-effectiveness.md`
- **Base:** `testing` at `fdc64b4abf3db262ea33efd0812b4208c654f57d` (merged E3)
- **Candidate branch:** `agent/indexing-e4-context-injection`
- **Environment:** Linux x86_64, GCC with `-Werror`, clang-format 19.1.7

## Contract exercised

The checked-in fixtures cover the full agent-facing path:

1. `/v1/code/context` reuses hybrid RRF candidates but requires one current project generation.
2. Exact/structural code leads, vector-only evidence must clear `0.70`, and at most four total
   code-plus-memory items are accepted.
3. Every code item carries project, path, current generation, freshness, confidence, provenance,
   and a line-or-file span.
4. Only exact active-project decision/constraint memory with an explicit path or symbol relationship
   to one accepted code result may be appended, and only after accepted code. Memory-only hybrid
   rows cannot make a task answerable; unanchored and shared/global memory fixtures are excluded.
5. An unrelated query returns `status=no_answer`, empty `results`, and empty `why` with HTTP 200.
6. The ingress formatter rejects wrong-project, stale, incomplete, oversized, or malformed packets,
   escapes resident text, and caps the rendered packet at 4,800 bytes (the initial 1,200-token
   budget).
7. At E4 delivery, `code_context_mode=observe` was the default and changed no model-visible bytes.
   E6 subsequently promoted `on`; `on` injects only
   on the first/new-task turn for an identified session; a related follow-up receives no repeated
   packet. Failed freshness, latency, availability, or answerability gates suppress `on` to
   observation/no packet rather than widening recall.
8. Legacy ingress code and every ingress memory call are resolved and scoped to the active project
   and workspace. If durable active-project identity is unavailable, the ingress issues no unscoped
   code or memory query. Diagnostic previews apply the protected project/workspace/global bucket
   order before their limit. Strict `on` mode also suppresses the legacy user/global typed-fact
   layer, so an abstained code packet cannot be replaced by global memory.

The legacy `/v1/code/hybrid` route is intentionally tightened when a project is supplied: its
memory leg now uses the same local-first visible reader instead of an unscoped fact lookup. Direct
legacy KB handler callers that supply no ambient or explicit scope keep their prior compatibility
semantics; the agent-facing ingress boundary is what resolves identity and refuses to issue those
unscoped calls.

## Commands and results

```text
make -C src build/obj/tests/unit-test-kb-http-routes \
  build/obj/tests/unit-test-ingress-preinject \
  build/obj/tests/unit-test-gw-stage-memory \
  build/obj/tests/unit-test-kb-client-search \
  build/obj/tests/unit-test-kb-client-memory \
  build/obj/tests/unit-test-workspace-memory \
  build/obj/tests/unit-test-config-defaults-golden \
  build/obj/tests/unit-test-config-surface -j2

src/build/obj/tests/unit-test-ingress-preinject
src/build/obj/tests/unit-test-gw-stage-memory
src/build/obj/tests/unit-test-kb-client-search
src/build/obj/tests/unit-test-kb-client-memory
src/build/obj/tests/unit-test-workspace-memory
src/build/obj/tests/unit-test-config-defaults-golden
src/build/obj/tests/unit-test-config-surface
src/build/obj/tests/unit-test-kb-http-routes
```

All eight binaries passed. The route fixture includes answerable, explicit no-answer, and
global/unanchored/memory-only exclusion cases plus stale/current generation fencing. The ingress
fixture includes strict metadata validation, observe/noninterference, first-task injection,
related-follow-up suppression, and task-change reinjection. The memory-client fixture verifies
active identity on all twenty ordered readers used by ingress/context surfaces; the adversarial
workspace fixture proves a low-confidence local diagnostic result cannot be crowded out by more
relevant global and other-project rows.

```text
make -C src kb server -j2
make -C src api-conformance-check v1-method-coverage-check
make -C src gen-sdks
make -C src sdk-parity-check
make -C src unit-tests TEST_RUN_JOBS=2
make -C src aimee-home-check
git diff --check
```

Both shipping binaries linked; KB API conformance found its routed subset of all 85 documented
endpoints present; method coverage passed; all 92 OpenAPI operations were present in all eight
generated SDKs; the complete unit suite passed after classifying the new field in the exhaustive
225-entry config descriptor inventory; and formatting and diff checks passed. Hosted CI is recorded
in the proposal acceptance record after the frozen-diff review and PR gate.

## Boundary and rollout ruling

This slice validates retrieval/packet mechanics, not causal task improvement. It deliberately ships
in `observe`; E6 owns the held-out precision/recall, latency distribution, coding-task, adoption,
and cost gates needed to promote `on`. The interrupted Ponytail artifacts remain historical red
evidence and are not spliced into this result.
