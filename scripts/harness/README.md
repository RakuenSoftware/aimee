# Harness analysis tools

Tooling for the **four-part harness** framing: every agent runtime is *loop +
tool interface + context management + control*. When an agent misbehaves it is
almost always one part, not "the model"; and as models improve, the parts that
bet against the model should shrink. See
[`docs/proposals/pending/four-part-harness-taxonomy.md`](../../docs/proposals/pending/four-part-harness-taxonomy.md)
for the full design, the subsystem map, and the durable-vs-delete classification.

Both tools are pure analysis — no build, no running aimee server required.

## `classify_failures.py` — attribute failures to one of the four parts

Reads execution-trace rows (the shape `src/trace_analysis.c` mines) and tags each
detected failure as **loop**, **tool**, **context**, or **control**, then prints
the distribution. Turns "context is our biggest tax" into a measured number.

```sh
python3 classify_failures.py --self-test            # red/green: one plan per part + edge cases
python3 classify_failures.py traces.json            # classify a trace dump
aimee trajectory export --json | python3 classify_failures.py -
python3 classify_failures.py traces.json --json     # machine-readable
```

Heuristics mirror the constants in `src/trace_analysis.c` (retry-loop threshold,
error indicators) so the in-process C port stays faithful. This script is the
reference that port must match.

## `delete_pressure.py` — rank scaffolding for deletion as models improve

Scores the per-tool prompt augmentations in `src/tool_prompts/` by per-turn token
tax and *doubt density* (imperative hedges that encode "the model won't do this
unless told"). High score = re-test against a current model, then likely trim.

```sh
python3 delete_pressure.py                          # scan src/tool_prompts/
python3 delete_pressure.py --json
python3 delete_pressure.py --anti-patterns export.json   # fold in runtime hit-rate
python3 delete_pressure.py --self-test
```

Doubt density is a deliberately weak, whole-word-matched heuristic (it counts
imperative hedges), not a measurement. Static pressure only **nominates**
candidates. The authoritative signal is whether *removing* a scaffold changes an
outcome — anti-pattern hit-rate (`--anti-patterns`,
needs `aimee guardrails anti-patterns export --json`) or a scaffold A/B against the
`tool` failure rate from `classify_failures.py`. The static score points the
camera; the runtime signal pulls the trigger.
