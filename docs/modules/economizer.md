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
predict cache residency. The proof planner in `proof.go` produces cost EVIDENCE only;
authorization requires a signed registry entry, and the production registry is empty by
design.

## Public contracts

Reduction is the main stage. Four narrow compatibility stages retire the last live C
implementations of algorithms already owned by Go: strict byte-preserving JSON compaction,
lossless spill recall, and process-local condensation telemetry. They expose no reduction
policy; they let C callers marshal bytes onto the event bus instead of keeping duplicate engines.

| Stage | Kind | Request | Response |
|---|---|---|---|
| `economizer-reduce` | 11009 | `{messages, system_prompt, seam, …config, state, turn}` | `{messages?, mutated, reason, …ledger, state}` |
| `economizer-json-compact` | 11010 | raw JSON bytes | result header plus compacted raw bytes |
| `economizer-tool-recall` | 11011 | bounded spill-directory/ref wire | result header plus recalled raw bytes |
| `economizer-tool-stats` | 11012 | empty | fixed process-counter snapshot |
| `economizer-record-build` | 11013 | session messages and range | record-derived files/errors/decisions plus Coordinate Closet |

The kind is fixed by the process contract at `4096 + ordinal*256 + stage`; economizer is
ordinal 27, so it is not a free choice.

`messages` travels as raw JSON in both directions and is emitted with the module's
cJSON-compatible printer, so the bytes the caller forwards are the bytes the fold
measured. A `messages` field absent from the response means nothing was mutated and the
caller must forward its **original** array untouched.

## Dependencies and consumers

The descriptor declares three dependencies and nothing else, because the module reads no
ambient state: its dependency set is the runtime it registers with plus the types it
exchanges.

- `config`: the shape of the resolved preset the caller sends in the request.
- `ir`: the message representation the prompt is assembled in.
- `module-runtime`: bus registration, stage dispatch and the process lifecycle.

Everything in C reaches it through one file, `src/modules/economizer/economizer_module_client.c`,
which owns request construction, event-bus calls, and reply decoding. Reduction uses the
shared JSON call helper; auxiliary byte-preserving stages use bounded binary frames.

| Consumer | Seam | Entry point |
|---|---|---|
| `src/posix/agent_runtime.c` | delegate (aimee's own agent loop) | `econ_module_reduce` |
| `src/modules/economizer/gateway_mutate_wire.c` | gateway (inbound `/v1` proxy) | `econ_module_reduce` |
| `src/posix/agent_runtime.c` | fresh authenticated tool result | `econ_module_json_compact` |
| `src/modules/tools/agent_tools_dispatch.c` | `tool_output_get` | `econ_module_tool_recall` |
| `src/server/server_state.c` | economizer dashboard | `econ_module_tool_stats` |
| `src/server/session_compact.c` | record-derived flashback | `econ_module_record_build` |

The client NEVER mutates its input array. `*reduced == NULL` means "use the original",
which is also what every failure path yields.
The dashboard's tool-condensation object includes `available`; zero counters from an
unattached module are therefore not presented as genuine zero activity.

## Providers and readiness

The module serves five bus stages and calls no provider itself, so it has no upstream to be
ready for. Readiness is binary and observed at the call site: `obs_bus_module_available`
reports whether an `aimee-module-economizer` process is attached to the bus.

When it is not attached, `econ_module_reduce` returns non-zero immediately and the caller
dispatches its original prompt. That is the designed steady state for any deployment that
has not enabled the module. A missing economizer costs tokens, never correctness.

## Configuration and activation

Every lever is default-off and resolved by the caller from `econ_preset`, so the module
reads no ambient config. The request carries the resolved values; the module applies them.

That includes the freeze cost guardrail, which takes the three provider **rates** rather
than a model name, so the pricing table stays with whoever owns it.

- `runtime_toggle.supported`: `false`. Activation is the presence of the module process,
  not a runtime flag, because flipping one mid-conversation would strand reducer state
  that the caller is still persisting across turns.

## Surfaces

There is no direct HTTP surface, MCP tool or CLI verb. The only in-process surface is the C client header
`economizer_module_client.h`.

The only remaining C files are the bus client and gateway connectivity seams; their exported
entry points only marshal requests to the Go process. They contain no reduction policy:
`gateway_mutate.c` decides whether a reported reduction is safe to dispatch, and
`gateway_mutate_wire.c` drives the snapshot/restore/retry dance around the provider call.
JSON compaction, tool condensation/recall, register parsing, and Coordinate Closet extraction
are entirely Go.

## Data and migrations

No schema, no tables, no migrations. Reducer state is not the module's to keep: the
freeze boundary, its `prefix_digest` and the page table travel in and out with each
request because the caller already persists them.

The one durable artifact is the **condensation spill store** on local disk, written by Go with
temp file, `fsync`, atomic rename. Spill refs are content-derived rather than sequential,
so the store is not enumerable by guessing. A state blob that cannot be read is discarded
rather than treated as fatal: the reduction still runs, starting from a cold freeze and an
empty page table, which costs one turn of cache warmth instead of failing the request.

## Security and privacy

Prompt content is the most sensitive thing aimee handles, and this module sees all of it.
It is therefore an in-process bus peer with no network egress of its own.

Ref validation is strict because a spill ref reaches the filesystem: anything that is not
a well-formed content-derived ref is refused rather than normalized. The proof registry is
the second control. `proof.go` can produce evidence that a transform pays for itself, but
applying one requires a signed registry entry, and production ships with an empty
registry, so no proof-authorized transform can run.

## Supported journeys

- **Delegate turn.** `agent_runtime` reduces before dispatch; a returned array replaces
  the prompt for that call only, and the ledger row records what was removed.
- **Gateway proxy turn.** `gateway_mutate_wire` snapshots the pristine body, reduces,
  and dispatches only if `gw_should_apply` returns `GW_BYPASS_NONE`, which requires a
  genuine net shrink AND a structurally clean result (no orphaned `tool_use`/`tool_result`
  pair, checked on a copy).
- **Measure-only turn.** The same path with mutation suppressed, so a deployment can see
  what reduction WOULD save before enabling it.

## Tests and failure behavior

The Go package carries the reduction tests, including the differential ones that pinned
the port against the C originals byte for byte. It also covers JSON compaction (including
invalid UTF-8), condensation/recall, counters, and every process-stage frame. On the C side
`unit-test-gateway-mutate` and `unit-test-gateway-mutate-wire` cover the seam decision and
provider retry dance.

Failure behavior is uniform and fail-open. Every failure mode returns non-zero and leaves
the caller's array untouched: bus unavailable, malformed reply, allocation failure, or a
reply whose `messages` will not parse. On the gateway path a payload-class `4xx` restores the
pristine array and resends once; a `5xx` trips the per-session breaker WITHOUT resending,
because provider state is uncertain after a server error.

## Operational diagnostics

Each reduction emits a ledger row with `baseline_tokens`, `reduced_tokens`,
`removed_tokens` and a reason, so savings are measured rather than assumed. A reduction
that is skipped is as informative as one that runs: `reason` distinguishes a measure-only
pass from "no gain available" from "already reduced".

The gateway seam additionally emits `gateway_hard_bypass{reason}` whenever it declines to
dispatch a reduced payload. A rising bypass count with a healthy reduce count means the
reducer is producing results the seam will not ship. Look at the reason label before
looking at the reducer.

## Compatibility

The bus kinds `11009` through `11013` are derived from the module's ordinal and stage, so they
are stable as long as the ordinal is. Reduction request and response bodies are JSON objects read field-by-field: an unknown field is
ignored, and an absent optional field takes its zero value, so a newer module and an older
caller interoperate in both directions.

The one field that is NOT optional in spirit is `messages`: its ABSENCE is meaningful
("nothing changed"), so a future version must never omit it to mean anything else.

## Extension and removal

A new lever belongs in the Go package behind the existing stage, not behind a new one. The
request already carries per-lever config, so adding a default-off field extends the
contract without a version bump; adding a stage would mean a second round trip for work
that shares the same fold.

Removal is equally undramatic: stop running the module process. Every caller already
treats its absence as "no reduction available" and dispatches the original prompt, which
is the same path exercised whenever the bus is momentarily unavailable. The C files that
would then be dead are `economizer_module_client.c`, `gateway_mutate.c` and
`gateway_mutate_wire.c`.
