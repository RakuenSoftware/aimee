# Live semantic context baseline

This directory records S0 of the live semantic context proposal. It exercises the shipping LSP
manager without enabling the proposed `lsp context` behavior.

The checked observation is intentionally red in several places. A green CI job means the recorded
baseline was reproduced, not that the live LSP path is production-ready. In particular, cold
diagnostics still report zero before a provider starts, Pyright definitions still expose the
client's missing `LocationLink` support, and Pyright references can be either zero or the complete
three-item set before background analysis finishes because Aimee has no document synchronization
barrier. No response proves saved-file freshness or worktree authority.

## Reproduce

Build the test probe, install the exact providers into a temporary tool root, then run:

```bash
python3 benchmarks/live-semantic-context/run_s0_baseline.py \
  --lsp-test src/build/obj/tests/unit-test-lsp \
  --gopls "$TOOL_ROOT/go/bin/gopls" \
  --pyright-langserver "$TOOL_ROOT/npm/node_modules/.bin/pyright-langserver" \
  --pyright "$TOOL_ROOT/npm/node_modules/.bin/pyright" \
  --assert-baseline
```

The pins are `golang.org/x/tools/gopls@v0.20.0` and `pyright@1.1.413`. CI installs them into the
runner's temporary directory on Linux and macOS, runs both real providers, validates the known
baseline, and retains one complete machine observation per platform. Missing binaries are recorded
as failures instead of successful empty results.

The runner samples the portable POSIX process table. The process count includes the probe and
provider children, and RSS is the peak sum across that tree on Linux and macOS. The in-process probe
records the first definition request as cold latency and the following reference request against
the same initialized provider as warm latency.

## Frozen S1 comparison

[`s1-task-manifest.json`](s1-task-manifest.json) contains 45 checked tasks against immutable Aimee
source commit `54e6d9093946100fed7f9ed214060a729ba6a0fa`: 30 semantic-eligible tasks and 15
controls. Semantic cells start from real use sites rather than definitions. They cover same-named
symbols, reference-backed localization, saved-file coordinate shifts, and two-anchor batches. Six
saved-file cells also carry typed missing, unsupported, timeout, crash, stale, and path-escape
overlays. Controls intentionally begin at direct definitions where ordinary file inspection should
remain optimal.

The agent system prompt and exact tool schemas are checked under `prompts/` and `tools/`. Their
SHA-256 values, the task-manifest hash, the model/reasoning/client execution contract, provider
versions, run order, and promotion thresholds are frozen in
[`s1-experiment-contract.json`](s1-experiment-contract.json). Validate all source coordinates,
oracles, counts, families, authority, failure outcomes, and content pins with:

```bash
python3 benchmarks/live-semantic-context/validate_s1_contract.py
```

## Interpretation

[`s0-baseline-observation.json`](s0-baseline-observation.json) is one immutable Linux observation,
not a performance distribution. The PR gate checks semantic outcomes and that timing/resource
fields exist; it does not assert the recorded millisecond or RSS values because shared runners are
noisy. The macOS matrix passed against both pinned providers on PR #2950, closing the last
pre-implementation S0 gate; the immutable run, job, artifact, and digest are recorded in the S1
experiment contract. The 20-clean-start availability trial and paired model study remain S1
promotion evidence, not prerequisites for freezing the comparison.

The TypeScript language server was also tried during S0. It did not complete the current client's
request sequence within a bounded 25-second probe. It is excluded from the deterministic two-server
gate and retained as compatibility evidence, not reported as ready.

## S1 candidate pin

The completed local candidate is immutable at
`d2257cbf569dba65b20b943b44e1549832793f3c`. It adds the grouped batched context operation, exact
saved-file `didOpen`/`didChange` synchronization, provider and document generations, whole-file
freshness hashes, workspace-relative containment, typed failures, and bounded source. The unit
contract directly exercises batching, truncation, stale source, unavailable providers, binary
input, and input/returned-path escapes. The required Linux/macOS job retains a separate S1
real-provider artifact after first reproducing the unchanged S0 baseline.
