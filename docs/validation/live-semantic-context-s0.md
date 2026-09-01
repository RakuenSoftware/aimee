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

The TypeScript language server 4.3.4 with TypeScript 5.9.3 was tried as an additional provider. It
did not finish the current client's request sequence within a bounded 25-second run. It is retained
as a compatibility observation and is not called ready or used to make the deterministic CI check
pass.

## PR enforcement

The `lsp-real-providers` CI job installs the exact gopls and Pyright pins, builds the native probe
and real Go config fixture, runs the redistributed Go and Python fixtures, and uploads the raw JSON
observation. Its result is folded into the protected `unit-tests` aggregate, so a pull request to
`testing` or `main` cannot pass that required context without exercising both real providers.

The benchmark smoke suite also validates the immutable observation, fixture hashes, known-red
classification, experiment stop state, and CI aggregation wiring.

## Decision

Do not implement S1 yet. The comparison design and promotion thresholds are frozen in
`s1-experiment-contract.json`, but candidate implementation remains closed until the checked 45-task
manifest, model endpoint and prompt hashes, and macOS provider observation are pinned. This is a
value safeguard: deterministic protocol work alone cannot establish that semantic context improves
coding outcomes over shipping Aimee and ordinary local inspection.

## Commands executed

```text
make -C src -j2 build/obj/aimee-module-config build/obj/tests/unit-test-lsp
src/build/obj/tests/unit-test-lsp
python3 benchmarks/live-semantic-context/run_s0_baseline.py ... --assert-baseline
```

The focused native suite passed under the same config-fixture environment used by the repository's
unit-test runner. The real-provider baseline matched the checked classifications.
