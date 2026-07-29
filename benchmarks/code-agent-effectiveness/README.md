# Agent-facing code-intelligence evaluation

This directory holds the attribution-safe red/green fixtures accepted by
[`agent-facing-code-intelligence-effectiveness`](../../docs/proposals/pending/agent-facing-code-intelligence-effectiveness.md).

`fixtures.json` is deliberately small and product-facing. It records five distinct failure classes:

- agent tool discovery does not find the blast-radius capability named by installed guidance;
- code lookups mix duplicate project namespaces instead of defaulting to the active project;
- Python path/import normalization leaves blast radius empty despite caller evidence;
- repository queries return unrelated global delegation episodes instead of abstaining; and
- a KB outage is not yet represented by the typed, recoverable result contract.

Each case preserves the untreated observation and the post-fix contract. Later slices reuse these
same fixtures; they must not replace the red observation with a passing treatment result.

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
