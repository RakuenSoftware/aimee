# Live semantic context baseline

This directory records S0 of the live semantic context proposal. It exercises the shipping LSP
manager without enabling the proposed `lsp context` behavior.

The checked observation is intentionally red in several places. A green CI job means the recorded
baseline was reproduced, not that the live LSP path is production-ready. In particular, cold
diagnostics still report zero before a provider starts, Pyright definitions still expose the
client's missing `LocationLink` support, and no response proves saved-file freshness or worktree
authority.

## Reproduce

Build the config fixture and test probe, install the exact providers into a temporary tool root,
then run:

```bash
python3 benchmarks/live-semantic-context/run_s0_baseline.py \
  --lsp-test src/build/obj/tests/unit-test-lsp \
  --config-module src/build/obj/aimee-module-config \
  --gopls "$TOOL_ROOT/go/bin/gopls" \
  --pyright-langserver "$TOOL_ROOT/npm/node_modules/.bin/pyright-langserver" \
  --pyright "$TOOL_ROOT/npm/node_modules/.bin/pyright" \
  --assert-baseline
```

The pins are `golang.org/x/tools/gopls@v0.20.0` and `pyright@1.1.413`. CI installs them into the
runner's temporary directory, runs both real providers, validates the known baseline, and retains
the complete machine observation as an artifact. Missing binaries are recorded as failures instead
of successful empty results.

The runner samples the complete process tree through Linux `/proc`. The process count includes the
probe and provider children, and RSS is the peak sum across that tree. The in-process probe records
the first definition request as cold latency and the following reference request against the same
initialized provider as warm latency.

## Interpretation

[`s0-baseline-observation.json`](s0-baseline-observation.json) is one immutable Linux observation,
not a performance distribution. The PR gate checks semantic outcomes and that timing/resource
fields exist; it does not assert the recorded millisecond or RSS values because shared runners are
noisy. The 20-clean-start availability trial, macOS evidence, task corpus, and paired model study
remain preconditions for an S1 promotion claim.

The TypeScript language server was also tried during S0. It did not complete the current client's
request sequence within a bounded 25-second probe. It is excluded from the deterministic two-server
gate and retained as compatibility evidence, not reported as ready.
