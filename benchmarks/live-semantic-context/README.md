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
experiment contract. The 20-clean-start availability trial and paired model study are S1 promotion
evidence, not prerequisites for freezing the comparison. The provider PR job now runs and retains
all 20 clean candidate starts for each pinned provider on both Linux and macOS; a missing or failed
start stays in the denominator.

The TypeScript language server was also tried during S0. It did not complete the current client's
request sequence within a bounded 25-second probe. It is excluded from the deterministic two-server
gate and retained as compatibility evidence, not reported as ready.

## S1 candidate pin

The completed local candidate is immutable at
`474bd69954237fca249eb44e942caeab4270ad5e`. It adds the grouped batched context operation, exact
saved-file `didOpen`/`didChange` synchronization, provider and document generations, whole-file
freshness hashes, workspace-relative containment, typed failures, and bounded source. The unit
contract directly exercises batching, truncation, stale source, unavailable providers, binary
input, and input/returned-path escapes. The required Linux/macOS job retains a separate S1
real-provider artifact after first reproducing the unchanged S0 baseline. Candidate probes also
record frozen runtime source tree `e6ba59ceba5a40323b006e834ac28ef39a2abc46`. The paired-study
instrumentation continues to require that exact tree. The PR gate separately requires the frozen
candidate to be an ancestor of the tested merge checkout and byte-compares the complete
semantic-context implementation/test surface against it. This permits unrelated files added by a
newer `testing` base while rejecting any drift in the evidenced feature, and the real providers
still execute against the actual merge checkout.
The first eligible model calibration found and fixed a cross-file false-stale defect before the
promotion run: bounded target source is now hashed against the target file before and after the
read, rather than against the anchor file. The epoch-2 PR lint gate then rejected formatting in that
repair. Evidence epoch 3 pins the mechanically formatted tree and restarts every promotion cell;
all earlier cells are retained as calibration and cannot contribute to a claim.

## Paired-study runner

`run_s1_paired_study.py` validates the candidate source tree, exact Codex CLI executable, model,
reasoning level, ripgrep, ast-grep, providers, and every instrumentation digest before dispatch. It
creates a clean non-detached clone at the task commit for every cell, applies only the manifest's
checked saved-file mutation, exposes the frozen arm through `s1_mcp_server.py`, retains raw Codex
JSONL and tool JSONL, and grades the structured result against the hidden oracle. The native
`s1_lsp_bridge.c` is linked to the same production `lsp_manager` and `lsp_context` objects as the
candidate unit gate; it is instrumentation, not a second semantic implementation.

Codex CLI 0.151.0 always adds `apply_patch` and generic MCP resource helpers when a local MCP server
is present. This was discovered before the first cell and is recorded as a pre-data amendment in
the experiment contract. All removable built-ins are disabled, the remaining identical helpers
are declared ineligible evidence, and their use invalidates a cell while remaining visible in the
raw event stream. Run without `--execute` for a no-inference lineage/order preflight; execution also
requires explicit paths to the digest-pinned Codex, ripgrep, ast-grep, gopls, and Pyright binaries.

Calibration also established two execution facts before promotion data. Provider-failure overlays
are not comparable to a healthy production arm, so the 135-cell paired value study now runs all 45
tasks under normal conditions and `--failure-suite` runs the six frozen overlays as 12 separate
LSP-only adversarial cells. Codex records one user turn for every `exec` cell, so that constant is
reported but cannot measure internal round trips; the preregistered paired efficiency endpoint is
tool calls plus end-to-end wall time. `summarize_s1_results.py` reports complete-pair arm metrics and
5,000-replicate paired percentile-bootstrap intervals while leaving the promotion decision
incomplete until the failure, cold-start, and checked reference-quality evidence are all present.

## S1 result

Evidence epoch 3 completed all 135 paired cells and 12 adversarial cells without infrastructure or
eligibility failures. Batched context was exact on 45/45 tasks, reached 96.67% semantic-task
adoption, reduced median semantic tool calls by 66.67% (paired 95% CI 50.00–66.67%), and reduced
median semantic wall time by 38.69% (paired 95% CI 33.27–41.80%). All controls remained exact with
zero median tool-call increase. Linux and macOS each passed 20/20 clean starts for both providers,
with complete checked references and no extras. The preregistered efficiency branch therefore
passes. Raw evidence, byte pins, the safety result, and the bounded interpretation are recorded in
[`live-semantic-context-s1`](../../docs/validation/live-semantic-context-s1.md).
