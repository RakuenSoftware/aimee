# Cross-repo dependency graph — acceptance validation

Live validation procedure for the cross-repo dependency graph proposal
([cross-repo-dependency-graph](../proposals/pending/cross-repo-dependency-graph.md),
§9). The **enumerated negative suite + stratified positive strata** are validated
in CI over the sqlite shim by
[`tests/test_cross_repo_acceptance.c`](../../src/tests/test_cross_repo_acceptance.c)
(end-to-end through `canonical_index_cross_repo_deps`) and the pure resolver /
classifier units in `tests/test_cross_repo_deps.c`. The gates below are the ones
that **must be measured live** against the real ~40-repo corpus on the `.254`
split stack — they need Postgres + the actual code index and cannot be screened
on the shim. This is the S9 runbook.

Prereqs: `aimee-server` + `aimee-kb` + `aimee-llm` running on `.254` with the
corpus indexed; `aimee index deps` / `aimee repo trust` reachable (the §S6/S7
CLI). Use `--json` for machine-checkable output.

## Gate #2 — known-true positive (HIGH, import-corroborated)

```
aimee index deps moonlight-qt --json
```

PASS iff the result contains an edge to **moonlight-common-c** at tier **≥ MEDIUM**
whose linking symbols include **`LiStartConnection`**, with evidence carrying:
call-site count, the resolved import path, `blocked_symbols_version`,
`distinctiveness_v`, `resolver_version`, and `repo_set_hash`. Spot-check the
reverse direction once it lands: dependents of `moonlight-common-c` include
`moonlight-qt`.

## Gate #3 — enumerated negative suite (100%) on the live corpus

Sample real instances of each class and confirm the tier:

| Class | Expectation |
|---|---|
| bare-name collisions (`init`, `get`, `run`, …) | **no** cross-repo edge |
| multi-definer, uncorroborated | **AMBIGUOUS** → review queue, no edge |
| conditional (`#ifdef`-guarded) import | capped at **MEDIUM** |
| dynamic import (`dlopen`/`import(...)`) | routed to **review** |
| untrusted-**caller** import corroboration | capped at **MEDIUM** (never HIGH) |
| untrusted-**definer** (a trusted caller imports/calls it) | **no edge** — an untrusted repo can't originate one (stricter than the literal §0 export-cap; verified end-to-end in `test_cross_repo_acceptance.c`) |

Vendored-copy: pick a `vendor/`/`third_party/` symbol duplicated across repos and
confirm it appears in the review queue:

```
aimee index deps <repo-with-vendored-copy> --review --json
```

Overflow: confirm the `--review` output carries `overflow.dropped` and the CLI
prints the overflow **WARNING** when the queue is saturated.

## Gate #4 — `--dry-run` offline inspection

> **Deferred.** `--reverse` and `--dry-run` were intentionally not shipped in S6
> (the kb canonical query is OUT-only and emits no per-stage pipeline detail yet);
> they land in the slices that implement reverse traversal and candidate emission.
> Gate #4 is validated when those ship — it is not a blocker for the P1 query path.

## Gate #5 — precision + recall floors (manual adjudication)

- **Precision:** sample **N = 50** random **HIGH** edges; manually adjudicate.
  PASS iff **≥ 95%** are true cross-repo dependencies, with **zero**
  corroboration-audit failures.
- **Recall:** against a hand-labeled ground-truth set stratified across
  {import-resolvable, call-site-only, vendored/header-only}, **≥ 50 sites/language**:
  HIGH recall **≥ 70%**, HIGH+MEDIUM **≥ 85%**.
- **AMBIGUOUS depth ≤ 10%** of the candidate universe.

Cross-check against direct SQL on `.254` postgres to confirm the emitted edge set
matches the labeled set and counts are sane.

## Gate #6 — latency + deployment

- `GET /v1/code/cross-repo-deps` + `aimee index deps` live on the `.254` plugin
  stack over the full corpus.
- **p50 ≤ 200 ms, p95 ≤ 2000 ms** for the forward query; capture `EXPLAIN
  (ANALYZE, BUFFERS)` for the working-set query and confirm the
  `idx_terms_name` / `idx_code_calls_callee` indexes are used (no seq-scan on the
  hot path).
- Deploy via the tierd lifecycle verbs (materialise + start / `update`), **not**
  `docker restart`.

On all gates passing, flip the proposal from `pending` to the completed state and
note the measured numbers here.
