# learning module

## Purpose and non-goals

Learning is required core because Aimee must improve from a user's corrections, choices, and repeated
work rather than behave as a stateless router. The module turns supported signals into reviewable and
bounded changes through `learning_router_record_signal`; it does not grant an LLM permission to rewrite
configuration, memories, skills, rules, or workflows without the applicable evidence and policy gates.

## Public contracts

`src/modules/learning/learning.h` defines signal inputs, dispatch results, proposals, actions, metrics,
and the router API. `learning_implicit.h`, `learning_bundle.h`, and `learning_evidence.h` cover detection,
evidence assembly, and candidate generation, while DB2 persistence currently remains in
`src/db2/db2_learning.h` and related source files as explicit physical-ownership debt.

The pointer-free learning-observation stage is now implemented by
`server-go/modules/learning` and shipped as the supervised Go
`aimee-module-learning` process. Its C `module_adapter.c` remains a parity
fixture for the unchanged `module_api.h` wire contract, with the pure C parity
policy isolated in `learning_signal_policy.c`. The production router has no
local signal-to-sink fallback: it requests the sink mask over event `6145` and
aborts before signal persistence when the process is absent or its response is
invalid. Proposal persistence, evidence assembly, and the broader learning
engine are still C migration work.

The descriptor declares the module-owned C implementation and parity adapter, the Go process handler
and tests, its headers, direct C tests, and this document; it sets `ownership_complete: true`.
`learning.h` and `module_api.h` are canonical public headers under
`src/modules/learning/include/aimee/learning/`. The bundle, evidence, implicit-detection, and C parity
policy headers remain private at the module root. Adding a new module-local source or header without
declaring it fails CI on `rule=ownership-complete`.

## Dependencies and consumers

- `config`: supplies learning thresholds, limits, synthesis provider settings, and policy gates.
- `ir`: supplies provider-neutral turn content from which supported implicit signals can be detected.
- `memory`: supplies evidence and receives approved durable improvements where the selected sink permits it.
- `module-runtime`: supplies required lifecycle contracts for the always-present learning capability.

Consumers include `cmd_learning.c`, `cmd_rules.c`, server/KB learning routes, the KB drain's candidate
synthesis lane, review workflows, dashboards, and agent execution that records explicit or implicit
feedback through the typed `learning_signal_input_t` contract.

## Providers and readiness

The deterministic router and proposal store are required; optional model-backed candidate generation
may be idle when its configured provider is unavailable. `learning_router_enabled` reflects operational
configuration for signal intake, but disabling one detector or sink is not equivalent to removing the
core learning module or its review/history contracts.

## Configuration and activation

- `runtime_toggle.supported`: `false`; learning remains part of core even when individual detectors or sinks are gated.

Settings under `learning.*` tune implicit detection, corroboration, caps, expiry, and synthesis commands.
The web configuration must expose a setting only when its detector, sink, or provider is compiled and
used, and must distinguish a temporarily idle provider from removal of required `learning` capability.

## Surfaces

User and operator surfaces include `aimee learning list|status|accept|reject|metrics`, KB/server learning
routes, proposal review, and dashboard metrics derived from `learning_metrics_commit_ratio` and
`learning_metrics_per_sink_caps`. Internal signal ingestion is also a surface because rules, turns, and
delegates rely on its result states and evidence references.

## Data and migrations

`DB2` tables store learning signals, proposals, evidence references, state transitions, and synthesis
work; schema and queries currently live under `src/db2`. Migrations must preserve proposal IDs, sink,
target, corroboration, expiry, and audit history so an old unresolved action cannot be replayed as an
unreviewed committed change after an upgrade.

## Security and privacy

Learning data can reveal preferences, mistakes, and working habits, so scope, identity, retention, and
memory PII controls apply before durable use. Actions parsed from `action_json` must pass sink-specific
authorization and caps; evidence is provenance for review, not authority to mutate another user's or
project's state.

## Supported journeys

An explicit correction or supported implicit event becomes a typed signal, is deduplicated and routed
to bounded sink proposals, and is then accepted, rejected, expired, or committed with evidence.
Operational metrics show whether `reranker`, `supersede`, `rule`, and `workflow` sinks are producing
useful changes without silently exceeding their weekly limits.

## Tests and failure behavior

The descriptor's two direct tests are `test_learning_bundle.c` (the evidence-bundle builder) and
`test_learning_metrics.c` (the router metrics through the public learning API). `test_learning_synth.c`
and `test_learning_version.c` exercise `kb/kb_learning_synth.c` and `kb/kb_learning_version.c`; they
carry the `learning` name and link `learning_bundle.o` as a dependency but are KB tests, so they are not
claimed here, the same way gateway does not claim the delivery-binary `test_gateway_*` tests.
`learning_implicit_replay.c` is a replay harness, not a `unit-test-learning-*` target. Together these
cover evidence, detection, proposals, metrics, candidate synthesis, and version behavior. Invalid
signals, unavailable storage, and unavailable learning-module classification fail closed. The router
metrics test verifies that a missing classifier returns before producing a signal or proposal result.
Optional synthesis failure leaves evidence/proposals inspectable and must not fabricate an accepted
action.

## Operational diagnostics

The `aimee learning metrics` surface reports commit ratios, per-sink cap utilization, and router/detector
latency from `learning_router_metrics`. Operators should correlate those counters with proposal states,
KB drain logs, provider readiness, and evidence references to distinguish no useful signal from a broken
ingestion or synthesis path.

## Compatibility

The `learning_signal_input_t`, proposal JSON, CLI verbs, route envelopes, sink names, and persisted state
machine are compatibility contracts. Relocating DB2 and root command code into the module must retain
those meanings; renaming a sink or reinterpreting `high_confidence` requires migration, tests, and a
surface-baseline decision.

## Extension and removal

Add a detector or sink by extending the typed router, evidence rules, caps, review journey, metrics, and
tests together. Self-contained experiments with no production caller should be deleted rather than
declared as learning. The core `learning` module cannot be removed; model-heavy candidate generation can
remain provider-gated without splitting the user-learning contract into a second optional subsystem.
