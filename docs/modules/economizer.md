# economizer module

## Purpose and non-goals

`economizer` owns **context reduction**: shrinking an assembled prompt before it goes to
a provider, without losing anything the agent will later need.

The levers are a rolling **history fold** (a skeleton of earlier turns plus a Coordinate
Closet that conserves exact identifiers verbatim), boundary-free **tool-body
compression**, deterministic **tool-output condensation** with a durable spill store, and
a **page table** that records what left the prompt so a later turn touching it can be told
the content is pageable rather than gone.

Two properties shape the whole module:

- **Reversibility.** Nothing is dropped without a way back. Folded detail stays in
  history, condensed output is spilled before the condensed body ships, and evicted
  coordinates are recorded so they can be paged in.
- **Byte-stability.** The folded prefix must stay byte-identical turn to turn or the
  provider prompt cache goes cold. That is why the module carries a cJSON-compatible
  printer rather than using `encoding/json`, which reorders object keys and HTML-escapes.

It does not decide *whether* to call a provider, does not talk to providers, and does not
predict cache residency. The proof planner produces cost EVIDENCE only; authorization
requires a signed registry entry, and the production registry is empty by design.

## Public contracts

One stage. `coord_closet`, `fold_recall`, `context_fold` and the condensation primitives
are internal to a reduction rather than separately callable, so exposing them would be
surface with no caller.

| Stage | Kind | Request | Response |
|---|---|---|---|
| `economizer-reduce` | 11009 | `{messages, system_prompt, seam, …config, state, turn}` | `{messages?, mutated, reason, …ledger, state}` |

The kind is fixed by the process contract at `4096 + ordinal*256 + stage`; economizer is
ordinal 27, so it is not a free choice.

`messages` travels as raw JSON in both directions and is emitted with the module's
cJSON-compatible printer, so the bytes the caller forwards are the bytes the fold
measured. A `messages` field absent from the response means nothing was mutated and the
caller must forward its **original** array untouched.

## State

The module is **stateless**. Per-conversation reducer state (the freeze boundary, its
prefix digest, and the page table) travels in and out with the request, because the
caller already persists it. A state blob that cannot be read is discarded rather than
treated as fatal: the reduction still runs, starting from a cold freeze and an empty page
table, which costs one turn of cache warmth instead of failing the request.

## Configuration

Every lever is default-off and resolved by the caller, so the module reads no ambient
config. That includes the freeze cost guardrail, which takes the three provider **rates**
rather than a model name — the pricing table stays with whoever owns it.

## Safety properties worth knowing

- The **fold freeze** pins a boundary so the prefix stays byte-identical, and guards it
  with a digest: a mid-run mutation of the folded region forces an epoch rather than a
  false reuse that would claim a warm cache it does not have.
- **Tool-output condensation is lossless-on-demand.** A condensed body ships only if the
  full output was durably spilled (temp file, fsync, atomic rename) and the recovery
  pointer fits. Spill refs are content-derived so they are not enumerable, and ref
  validation is strict because the ref reaches the filesystem.
- A family filter **passes output through verbatim** when a command failed with no
  recognisable failure signal. Condensed output that hid why a command failed is worse
  than long output.
- The **gateway path** snapshots before it reduces and never dispatches a payload it
  cannot restore. A 4xx restores the pristine array and resends once; a 5xx trips the
  per-session breaker without resending, because provider state is uncertain after a
  server error.
