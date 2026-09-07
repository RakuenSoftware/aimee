# Live semantic context S0 baseline

**Date:** 2026-09-01

**Runtime baseline:** `cccb1490560810a3da92691f381f9ab59040bb7c` (`origin/testing`)

**Behavioral scope:** no new agent or runtime semantic behavior

## Result

S0 establishes that Aimee's current LSP path is useful as an experiment substrate but is not ready
to promote. One pinned provider passed the existing location contract, a second exposed a protocol
shape the client does not parse, and the operational measurements are high enough that the S1 value
bar must include startup and process cost.

| Provider | Definition | References | Cold definition | Warm references | Peak process-tree RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| gopls v0.20.0 | 1 intended target | 3/3 | 64 ms | 29,999 ms | 659,224 KiB |
| Pyright 1.1.413 | 0, `LocationLink` unsupported | 3/3 | 191 ms | 320 ms | 698,572 KiB |

These are single observations from a Linux host, not latency distributions. The CI gate asserts
semantic classifications and records timing and resource fields, but does not use these noisy
single-run values as performance thresholds.

Both cold diagnostic probes returned zero diagnostics while reporting zero active providers. This
reproduces the false-empty state: `lsp_manager_diagnostics` reads stored notifications and does not
start a server. The probe also confirms that the current call accepts the workspace path selected
by its caller. Source inspection preserves the other red conditions: no saved-document version or
hash, no detached-worktree routing, text-shaped timeout/crash failures, and unsupported Windows
execution.

The first two PR runs added a second Pyright observation: the same pinned provider and fixture
returned either all three references or zero references depending on whether its background
analysis completed before the request. Definition remained empty in both runs because the client
does not parse `LocationLink`. The PR contract accepts only those two known reference states and
keeps the combined result classified as unsynchronized and unsupported. It does not retry until a
preferred answer appears.

The TypeScript language server 4.3.4 with TypeScript 5.9.3 was tried as an additional provider. It
did not finish the current client's request sequence within a bounded 25-second run. It is retained
as a compatibility observation and is not called ready or used to make the deterministic CI check
pass.

## PR enforcement

The `lsp-real-providers` CI matrix installs the exact gopls and Pyright pins on Linux and macOS,
builds the native probe, configures its existing in-process config-contract peer, runs the
redistributed Go and Python fixtures, and uploads one raw JSON observation per platform. Its
combined result is folded into the protected
`unit-tests` aggregate, so a pull request to `testing` or `main` cannot pass that required context
without exercising both real providers on both claimed platforms.

The benchmark smoke suite also validates the immutable observation, fixture hashes, known-red
classification, experiment stop state, and CI aggregation wiring.

## Decision

The macOS matrix leg passed on PR #2950 at head `a415b76208245612fbfe58b5d695285b3e2b5ee3`.
Workflow run `33520936934`, job `99900203992`, retained artifact `9805765992`, and its SHA-256 digest
are pinned in `s1-experiment-contract.json`. The artifact proves both real providers started on
macOS, gopls returned the intended definition and all three references, and Pyright reproduced the
predeclared `LocationLink` limitation while returning all three references. This closes S0 and
authorizes S1 candidate implementation.

The comparison design, 45-task checked corpus, model execution contract, system prompt, tool
schemas, provider versions, run order, and promotion thresholds were frozen before authorization.
The corpus uses real use-site anchors with independent definition oracles instead of
definition-to-itself probes. This remains the value safeguard: deterministic protocol work alone
cannot establish that semantic context improves coding outcomes over shipping Aimee and ordinary
local inspection. The completed candidate implementation commit must be pinned before the first
candidate-arm cell runs.

## Commands executed

```text
make -C src -j2 build/obj/tests/unit-test-lsp
src/build/obj/tests/unit-test-lsp
python3 benchmarks/live-semantic-context/run_s0_baseline.py ... --assert-baseline
```

The focused native suite passed under the same config-fixture environment used by the repository's
unit-test runner. The real-provider baseline matched the checked classifications.
