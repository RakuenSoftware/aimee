# Harness analysis

The harness model has four parts: loop, tools, context, and control. Diagnose a failure against the
part that owned it instead of treating every failure as a model problem.

## Classify failures

```bash
python3 scripts/harness/classify_failures.py --self-test
python3 scripts/harness/classify_failures.py traces.json
aimee trajectory export --json | python3 scripts/harness/classify_failures.py -
```

The classifier mirrors the in-process trace heuristics and can emit JSON. It is attribution, not a
causal proof; inspect the tagged trace before changing the harness.

## Find deletion pressure

```bash
python3 scripts/harness/delete_pressure.py
python3 scripts/harness/delete_pressure.py --json
python3 scripts/harness/delete_pressure.py --anti-patterns export.json
```

The score finds high-token prompt scaffolding built around assumptions about weak models. It only
nominates an A/B test. Delete a scaffold when measured outcomes stay flat or improve without it.

The design record is
[Four-part harness taxonomy](../../docs/proposals/done/four-part-harness-taxonomy.md).
