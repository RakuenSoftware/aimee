# Fold pipeline-order spike (P1.5)

De-risks the P2 rolling-fold + fold-freeze implementation by pinning **where** the
transcript fold runs and **how** it contributes to the single cache-prefix owner,
before any P2 code is written. Companion to `fold_budget.{c,h}` (the §7 resolver
landed in the same slice). Ruling from the plan roundtable: target the **delegate
path first** (it already owns `payload_rewrite`); the ingress proxy path is a later
slice.

## Normative pipeline order (delegate path)

`agent_runtime.c` is the actor throughout: it calls each step and owns the buffers.
Six steps per turn in `agent_execute_with_tools_internal()`
(`src/posix/agent_runtime.c`); step 1 is pre-stage input, steps 2–6 are actions:

1. **raw DB1 transcript** — append-only `messages` cJSON array (never mutated).
2. **session compaction** — `maybe_compact_before_request()` (≈`agent_runtime.c:677`).
3. **[FOLD PASS — P2]** — runtime calls `context_fold_view(messages, sys, &budget,
   cfg, &out)`, which **produces and returns** the synthetic skeleton pair +
   Coordinate Closet (§2) as a view, non-destructively (`messages` untouched), then
   exits. The fold function does not touch prefix state.
4. **provider-shape normalization** — runtime renders the step-3 view into the active
   provider shape (Anthropic content blocks / OpenAI `tool_calls` / Gemini `parts`)
   into a **runtime-owned byte buffer**, with the **atomic tool-pair** rule (a
   `tool_use`+`tool_result` fold together or not at all; folded regions are plain
   non-tool text). This is a deterministic 1:1 render, not a semantic rewrite.
5. **cache-prefix registration + single hash** — runtime registers the step-4
   buffer `(ptr,len)` via `payload_rewrite_register_span()`, then the canonical
   prefix-hash wrapper (`track_anthropic_payload_rewrite()` and its
   `track_{openai,gemini}_payload_rewrite` siblings, all delegating into
   `payload_rewrite.c`, ≈`agent_runtime.c:698`) hashes the registered spans **once**.
6. **request build** — `agent_build_request_*()` (≈`agent_runtime.c:699`).

Invariant: step 3's output is **semantically frozen**; step 4 is a deterministic
1:1 render; the exact post-step-4 bytes are what step 5 registers and hashes — no
semantic change *and* no byte change is permitted between steps 4 and 5. "Rewrite"
means semantic mutation; rendering at step 4 is not a rewrite. The fold pass MUST
sit at step 3 (after compaction, before the step-5 hash).

## Single prefix-state owner + span registry

`payload_rewrite.c` stays the sole owner of the FNV-1a prefix hash and
`payload_rewrite_state_t`. The fold does not hash anything itself. New **internal**
API (declared in a `payload_rewrite_internal.h`, or enum-keyed from a closed set):

```c
/* Register a stable span that contributes to the cache prefix, in order.
 * State-scoped to the turn's payload_rewrite_state_t (not process-global) so
 * nested/concurrent/retried turns cannot leak spans into each other.
 * ptr must remain valid (runtime-owned, see step 4) until the matching
 * payload_rewrite_spans_reset(). Returns 0 on success, -1 on a misuse the
 * registry can detect: bad/duplicate id, NULL ptr, zero len, or out-of-order
 * registration. A debug build additionally asserts each (ptr,len) is still
 * resident at hash time. */
int payload_rewrite_register_span(payload_rewrite_state_t *st, payload_rewrite_span_id_t id,
                                  const char *ptr, size_t len);
/* Mandatory + idempotent at the top of each turn. */
void payload_rewrite_spans_reset(payload_rewrite_state_t *st);
```

- `fnv1a_update` stays `static` in `payload_rewrite.c`.
- The registered span is the **post-step-4** (provider-shape-normalized) serialized
  bytes — exactly what is sent — so the hash matches what the provider caches.
  `agent_runtime.c` owns that buffer; the fold (step 3) only produces a view and
  exits. Lifetime: register after step 4, hash at step 5, `spans_reset()` at the top
  of the next turn — no use-after-free / double-free.
- The pending `ingress-compression-and-cache-alignment` work registers the envelope
  span through the same API — **serialized convergence**: the shared span-registry
  PR lands first, then the fold and ingress features layer on it. Never two writers.

## CI single-owner enforcement

A lint scan fails the build if any new caller of `fnv1a_update` or a
`payload_rewrite_state_t` mutator appears outside `src/payload_rewrite.c`,
mechanically preserving the single-owner contract the plan roundtable required (B7).

**Bootstrap gap:** the lint lands in the *same* P2 PR that introduces the registry,
so that PR is not gated by its own lint at review time. The P2 registry PR must
therefore be reviewed specifically for any new `fnv1a_update` caller outside
`payload_rewrite.c`, and ship the `git grep -nE '\bfnv1a_update\b' src/ | grep -v
payload_rewrite` CI step from day one.

## Budget config surface (deferred to P2)

`fold_budget_resolve()` is a pure function of `(model_id, fold_budget_config_t)`.

**Explicitly NOT in P1.5** (intentionally deferred to P2, so a follow-up author does
not "helpfully" wire inert config that nothing reads):

- `config_t` fields + a `fold` config section for the five knobs: window fallback,
  retained-band %, tail-cap %, pressure-ceiling %, prefix-saturation %, and the
  closet budget;
- a `fold_budget_config_t`-from-`config_t` populator at the step-3 call site.

These land in P2 in the same change that consumes the resolver. **P2 acceptance
requires** that wiring plus determinism tests for the B-S8 rule: any per-tool
override that still affects a fold is frozen for the freeze TTL or serialized into
the fold-input hash, so identical inputs cannot resolve to two budgets across turns.
