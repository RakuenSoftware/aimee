# Lossless JSON economizer transform and activation gate

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

> **Superseded for runtime policy (2026-07-23):** the exhaustive monetary-proof gate described
> below is retained as research history. The shipped interface is now `off`, `safe`, and
> `aggressive`. Safe admits only deterministic strict-JSON whitespace compaction at the fresh local
> tool-result boundary; aggressive owns lossy reduction. See
> [the economizer overview](../../features/economizer.md).

- **State:** DONE. Delivered scope archived 2026-07-26.
- **Review status:** ROUND TABLE APPROVED (3/3, zero findings)
- **Date:** 2026-07-22
- **Normative gate:** `provider-neutral-economizer-safety-spec.md`

## Decision

Implement the reusable transform, provenance, registry-serialization, and dispatch-lease machinery,
but do not add an OpenAI or Anthropic entry to the production transform registry yet. A dormant
transform is not reported as a saving. The registry remains empty until the provider evidence in the
activation matrix below is authoritative and pinned.

This is not a conclusion that economization can never save money. Compacting large new JSON tool
results, changing only an unprotected suffix after an explicit cache breakpoint, and avoiding a
long-context price boundary remain valid candidate classes. The current limitation is proof: the
system cannot guarantee the strict cost inequality for a real request using the contracts presently
exposed by either provider.

## Transform contract

The first candidate transform removes only RFC 8259 whitespace outside JSON strings from a locally
produced JSON tool result. It preserves every other input byte, including object-member order, number
lexemes, string escapes, Unicode normalization, and duplicate-key spelling. It rejects malformed
JSON, duplicate object-member names after JSON unescaping, invalid UTF-8, non-scalar Unicode escapes,
excessive nesting, and an output that is not shorter.

Admission requires a one-shot provenance capability issued by authenticated local tool execution. It
binds tenant, task, call, source bytes, semantic contract, transform, and transform version. Import,
persistence, replay, recall, rehydration, process transfer, or ambiguous ownership invalidates the
capability. Capability material and content digests are never logged.

Both pristine and candidate provider requests must be completely serialized before planning. No
cache control, route, model, service tier, output limit, header, field order, or protected byte may
change. The transform is deterministic, but determinism and JSON data-model equivalence are only
admission facts; neither is a cost proof.

The signed registry tuple also binds a scenario-set identity and exact coverage bitmap. Every
scenario ID must appear exactly once, the proof count must equal the signed coverage cardinality,
and the candidate upper bound plus margin must be below both its paired baseline lower bound and
the minimum baseline lower bound across the complete set. A caller cannot authorize a favorable
subset of the signed cache/pricing outcomes.

## Provider activation matrix

| Required proof fact | OpenAI GPT-5.6 | Anthropic Claude | Activation |
|---|---|---|---|
| exact final request count | `/v1/responses/input_tokens` is documented as the exact count received by the model; local request-structure tokenization is not exact | token-count endpoint is documented as an estimate; tokenizer may change | OpenAI count eligible only with bounded preflight cost; Anthropic blocked |
| economizer preflight cost | count-endpoint billing is not authoritatively stated as zero or given a finite maximum | count endpoint is documented as free | OpenAI blocked; Anthropic insufficient alone |
| immutable model/tokenizer binding | `gpt-5.6` is an alias for Sol and the public Sol page does not publish a dated snapshot/tokenizer contract | no exact public tokenizer contract | blocked |
| finite counting error | none needed for an exact response, but the remote preflight itself must be included | no finite error bound is documented | blocked |
| cache boundary/state | GPT-5.6 documents `prompt_cache_options.mode: explicit`; no-marker requests disable caching, and marked prefixes are explicit, but all protected rendered content still must be proven identical | explicit markers are documented; automatic top-level placement, server-tool breakpoints, and lookback must be excluded | eligible only for explicit-only layouts |
| complete-call output bound | preserved client limit or documented hard maximum, priced at the worst applicable rate | same | request-dependent |
| long-context threshold | strict `>272000`; full-request multipliers apply | model- and contract-specific | only after exact pinned counts |

OpenAI remote counting is exact token evidence, but exact tokens alone do not prove a cost. The
count is not used on the live path unless its own marginal charge is authoritatively bounded and included in every
candidate scenario and the safety specification is amended to permit that preflight. The planner
models this current state explicitly as `remote_token_count_unpriced` and rejects it. Anthropic's free
estimate cannot authorize a strict inequality without a finite error bound. Returned usage is
settlement evidence for the request sent, not proof of the counterfactual pristine request.

## Atomic dispatch lease

A non-empty registry requires a tenant-scoped generation state protected by a read/write lock. The
transport acquires a read lease after connecting and immediately before emitting any request header,
validates tenant, account, registry, pricing, contract, tokenizer, cohort, and kill-switch
generations, selects pristine or candidate bytes, emits the first request write while the lease is
held, then releases it. Invalidation takes the write lock. This is the linearization point; a check
merely performed before calling the HTTP layer is insufficient.

Once the first write occurs, retries may reuse only the identical selected snapshot. No retry may
re-plan, switch representations, restore, or resend the alternative. A failed or stale lease selects
pristine before any byte leaves the process.

## Activation checklist

An individual provider entry may be signed only when all of these are true:

1. exact model snapshot, tokenizer, serializer, pricing, cache, and endpoint contracts are pinned;
2. counting is local and exact, or a separately approved provider preflight has a finite total cost
   included in every scenario;
3. the complete candidate-call upper bound plus margin is below the pristine lower bound in every
   shared scenario, including zero baseline output and maximum candidate output;
4. provenance, JSON, cache-prefix, cross-tenant, generation-race, retry-byte, threshold, and settlement
   tests pass for that exact tuple;
5. live canaries reconcile provider usage and billing without exceeding the authorized bound; and
6. a separate roundtable review converges on the exact signed registry entry.

Only the authenticated internal provider planner may construct a proof or assign scenario IDs. No
public request field, plugin, recalled artifact, or caller-supplied cost bound may populate the
authorization object. Production first-write call sites must use the scoped dispatch-lease guard;
raw begin/end calls are reserved for tests and must share one cleanup label.

Until then, proof-gated mode continues to send the pristine request and produces no savings claim.

## Review and validation record

The configured `e2efix` roundtable first identified four blocking hardening gaps. The implementation
added scoped lease cleanup, detached Ed25519 tamper tests, signed exhaustive scenario coverage, and a
direct proof-gated pristine-byte parity test. The follow-up review converged with three participants,
zero failures, no degradation, and zero findings (artifact
`33863eed117bdb09e85695c6e8a7498537726f8a6775e12d1df148fa6648b77d`).

Validation completed on the merged-forward branch includes:

- full lint, focused OpenAI/Anthropic/proof/config/wire/CLI tests, and ASAN/UBSAN activation tests;
- fresh Optane-backed Debian 13 CT 265 builds and the complete focused suite;
- fresh provider CT 266 byte capture showing identical off/proof-gated OpenAI and Anthropic request
  bodies on their production routes;
- a real GPT-5.6 paired canary through the configured ChatGPT/Codex delegate transport returning
  identical output and provider-reported usage: 403 prompt tokens and 7 completion tokens in both
  modes. This is pass-through evidence, not OpenAI API billing or `/responses/input_tokens` evidence.

No real Anthropic billing canary was available from the configured credentials. That is not an
activation exception: the Anthropic registry remains empty, so the production behavior is pristine
pass-through and makes no savings claim.

## Provider-contract refresh, 2026-07-23

The activation matrix was rechecked against the then-current official provider documentation. The
important OpenAI change is that GPT-5.6 now documents both explicit cache breakpoints and an exact
input-token endpoint. Those facts resolve the earlier questions of whether a breakpoint can be
identified and whether a complete structured request can be counted exactly. They do not resolve
the monetary proof: OpenAI does not document the count endpoint as free and publishes no finite
maximum charge for it, while the public Sol model page does not expose a dated immutable snapshot or
tokenizer identity. Anthropic still documents its free count as an estimate with no numerical error
bound. The signed production registry therefore remains empty.

Authoritative references checked for this refresh:

- <https://developers.openai.com/api/docs/guides/token-counting>
- <https://developers.openai.com/api/docs/guides/prompt-caching>
- <https://developers.openai.com/api/docs/models/gpt-5.6-sol>
- <https://platform.claude.com/docs/en/build-with-claude/token-counting>
- <https://platform.claude.com/docs/en/build-with-claude/prompt-caching>

### Activation decision

No OpenAI or Anthropic production tuple is authorized by the evidence above. The signed production
registry must remain empty, and `proof_gated` must remain byte-identical pristine pass-through, until
the complete-call inequality can be proved for every supported request. In particular:

- OpenAI remote input counting is qualification-only. It must not be added to the production request
  path unless OpenAI publishes a binding zero-cost contract or finite maximum charge for that call
  and that cost is included in both sides of the proof.
- Anthropic remote input counting cannot authorize a production transform until Anthropic publishes
  an exact applicable tokenizer contract or a finite numerical error bound that is included in the
  proof.
- Provider documentation, canaries, and remote counting experiments are evidence-gathering work.
  They cannot populate or sign the production registry by themselves.
- Reconsidering activation also requires an immutable model/tokenizer binding, protected cache-byte
  identity, complete request/output bounds, exhaustive pricing scenarios, and signed fixture proofs.

Failing any planner requirement is not a degraded economization mode. It selects the original request
with no economizer preflight, retry, cache mutation, or transform. A damaged registry signature or an
unexpected nonempty production registry instead fails the wire fence before provider dispatch, so it
cannot add provider cost.
