# The aimee Economizer

The economizer is fail-closed: it may change a provider request only when a local,
provider-specific proof establishes a strict call-level cost reduction. If any required fact is
unknown, the original provider body is sent unchanged.

```yaml
economizer:
  mode: off             # off | proof_gated (default: off)
```

```sh
aimee config set economizer.mode proof_gated
```

## Current release behavior

The live transform registry is empty. Therefore both modes currently send the same pristine request:

- `off` bypasses economizer registry validation and the snapshot allocation.
- `proof_gated` validates the signed empty registry, then copies the final provider body into an
  immutable wire snapshot.

The snapshot's pointer and exact byte length are passed to the HTTP transport and retained for every
ordinary retry. A retry can duplicate delivery after an ambiguous network failure, but it cannot
rebuild, restore, or substitute a different economizer representation.

The release also contains dormant activation machinery for a future reviewed transform: strict
whitespace-only JSON compaction, one-shot local-output provenance, canonical signed-registry tuple
serialization, and a tenant-scoped first-write generation lease. Source-surface tests keep those APIs
unreachable from live request paths while the registry is empty. Their presence is not reported as a
token or dollar saving.

This release deliberately removes the previous live history folding, tool-output condensation,
body compression, gateway mutation, and economizer-owned restore/resend behavior. Their helper code
may remain for offline or isolated tests, but it has no production request caller. The economizer
also does not add, remove, or move OpenAI or Anthropic cache controls.

## Why the planners are provider-specific

OpenAI and Anthropic both reward stable cacheable prefixes, but their cache breakpoints, write/read
prices, long-context rules, and response accounting differ. GPT-5.6 exposes explicit breakpoints and
an exact remote input count, but the counting call has no published finite price bound. Anthropic's
free remote count is explicitly an estimate with no published numerical error bound. Neither API
therefore exposes enough pre-dispatch cost evidence to authorize a generic compressor safely.

The OpenAI and Anthropic planners therefore use separate signed pricing and cache-semantics
contracts. They operate only on local evidence and fully serialized alternatives. An OpenAI remote
exact count is rejected as `remote_token_count_unpriced`; an Anthropic remote estimate is rejected as
`remote_token_count`. Cache probes, predicted cache hits, and post-response usage fields likewise
cannot authorize a change.

The planners can return a proof for reviewed fixtures, but they are not yet connected to a live
transform because the production registry has no entries. A future transform needs its own lossless
semantic contract, exact tokenizer/model compatibility, provenance rules, and converged review
before it can enter that registry.

This is an activation decision, not a claim that savings are impossible. With the current provider
contracts, no OpenAI or Anthropic tuple has a complete worst-case monetary proof. Both production
registries therefore remain empty; unsupported, unknown, or incomplete evidence sends exactly the
original request without a counting preflight or economizer retry. A damaged signature or unexpected
nonempty registry fails the wire fence and sends no provider request.

## Configuration migration

The old scalar values `economizer: safe` and `economizer: aggressive`, and the old
`economizer.enabled` / `economizer.aggressive` object, are unsupported. They are not mapped because
either mapping could silently activate behavior the operator did not select. Replace them explicitly
with:

```yaml
economizer:
  mode: off
```

or, to enable the proof fence for future reviewed transforms:

```yaml
economizer:
  mode: proof_gated
```

An omitted setting defaults to `off`. Explicit legacy or malformed values make configuration loading
fail instead of falling back to an active mode.

## Claims this release does not make

- It does not claim that generic compression, summarization, recall, or rehydration saves money.
- It does not claim completed-task savings from a cheaper individual call.
- It does not claim knowledge of provider cache residency or undocumented breakpoints.
- It does not claim exactly-once delivery across transport failures.
- It does not report hypothetical savings as realized savings.

The normative safety rules and implementation plan are in
`docs/proposals/pending/provider-neutral-economizer-safety-spec.md` and
`docs/proposals/pending/provider-neutral-cache-aware-economizer.md`. The current provider activation
matrix is in `docs/proposals/pending/economizer-lossless-json-live-transform.md`.
