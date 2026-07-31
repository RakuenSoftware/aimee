# Agent-facing code-intelligence evaluation

This directory holds the attribution-safe red/green fixtures accepted by
[`agent-facing-code-intelligence-effectiveness`](../../docs/proposals/done/agent-facing-code-intelligence-effectiveness.md).

`fixtures.json` is deliberately small and product-facing. It records five distinct failure classes:

- agent tool discovery does not find the blast-radius capability named by installed guidance;
- code lookups mix duplicate project namespaces instead of defaulting to the active project;
- Python path/import normalization leaves blast radius empty despite caller evidence;
- repository queries return unrelated global delegation episodes instead of abstaining; and
- a KB outage is not yet represented by the typed, recoverable result contract.

Each case preserves the untreated observation and the post-fix contract. Later slices reuse these
same fixtures; they must not replace the red observation with a passing treatment result.

Treatment records are additive. The first is
[`docs/validation/agent-facing-code-intelligence-e1.md`](../../docs/validation/agent-facing-code-intelligence-e1.md),
covering capability discovery, schemas, installed guidance, and active-project request defaults.

Validate the checked-in structure without a live KB:

```bash
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py
```

The interrupted Ponytail run is diagnostic evidence only. Minimal, non-secret extracts are tracked
under `evidence/` and pinned by `fixtures.json`; the complete raw streams are not vendored. Their
checksums, exact original locations, versions, exclusions, and reproduction commands are recorded in
[`docs/validation/agent-facing-code-intelligence-red-baseline.md`](../../docs/validation/agent-facing-code-intelligence-red-baseline.md).

Verify the tracked evidence bytes as well as the fixture schema:

```bash
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py --verify-sources
```

Fresh experiment matrices use `checkpoint_runner.py`. A new run requires a JSON plan,
an unused artifact directory, and a stable checkpoint name:

```bash
python3 benchmarks/code-agent-effectiveness/checkpoint_runner.py \
  --plan matrix.json --run-dir artifacts/e6-20260730 --checkpoint full-matrix
```

Resume by omitting `--plan` and naming the same run directory and checkpoint. The
runner rejects run/plan/checkpoint provenance mismatches, never overwrites a prior
run or cell attempt, and records command failures/timeouts as `infrastructure-invalid`
with `score_eligible:false` while preserving stdout, stderr, timing, and return code.
Invalid cells must not enter quality denominators and must be rerun as new attempts;
the checkpoint does not advance past an invalid cell, and results from another run
are never copied into a checkpoint.

E6 adds a 16-case retrieval corpus, eight paired coding-task definitions, four predeclared arms,
and a versioned agent prompt. Score a result envelope with:

```bash
python3 benchmarks/code-agent-effectiveness/e6_evaluate.py \
  benchmarks/code-agent-effectiveness/results/e6-20260730.json
```

The scorer fails closed for promotion: all four coding arms need eligible fresh cells, the `on`
arm needs at least 80% pre-edit packet actuation, and confidence/efficiency gates must pass. The
first pinned E6 record passes deterministic retrieval but contains no eligible provider-backed
paired cells, so it retains the shipping `observe` default and delegates that bounded experiment to
the linked residual proposal.

The fresh follow-up is `results/e6-20260730-provider.json`, produced from the complete immutable
`e6-provider-plan.json`; its generated summary is `results/e6-20260730-provider-summary.json`.
All 32 cells are eligible and the scorer returns `promote-on`. Raw streams, patches, and checkpoint
evidence remain at the exact durable locations recorded in the E6 validation report.

Regenerate and audit that promotion verdict with the committed fail-closed scorer:

```bash
python3 benchmarks/code-agent-effectiveness/e6_evaluate.py \
  benchmarks/code-agent-effectiveness/results/e6-20260730-provider.json \
  --output benchmarks/code-agent-effectiveness/results/e6-20260730-provider-summary.json
```

The committed provider plan is an immutable manifest of the original operator environment, so its
absolute runner/artifact paths are evidence rather than a portable launch configuration. Regenerate
the plan to replay elsewhere. The provider runner is trusted-operator-only: it copies provider auth
into the disposable Codex home and passes both approval/sandbox and hook-trust bypass flags. A cell
can therefore exercise the operator account and network for its process lifetime; captured streams
and patches audit the disposable workspace, not every external side effect. Run it only on an
authorized isolated host with scoped credentials. `AIMEE_E6_CODEX`, `AIMEE_E6_CODEX_AUTH`, and
`AIMEE_E6_FIXTURE_HARNESS` override the recorded operator defaults.
