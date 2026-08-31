# Deep-Curator fixture corpus

Labeled fixtures for the deep-curator extraction passes, used by the curator
eval oracles (`doc_precision`, `code_agreement`, `bridge_precision`) and by
`src/tests/test_curator_fixtures.c`. Implements the architecture charter's
"Fixture families" requirement, every fixture set exists in **three shapes**:

| File | Shape | Meaning |
|------|-------|---------|
| `fixtures/positive.jsonl`       | positive       | cases a pass should handle correctly |
| `fixtures/false_positive.jsonl` | false_positive | cases that look triggerable (a rejection / stale-mark / promotion) but must **not** fire |
| `fixtures/regression.jsonl`     | regression     | real past mistakes converted to labeled fixtures so the same error cannot return |

Each line is one standalone JSON object (JSON Lines).

## Common keys

| key | type | notes |
|-----|------|-------|
| `id` | string | unique within the corpus, e.g. `code-03` |
| `pass` | string | `extract_doc` \| `extract_code_unit` \| `bridge` |
| `shape` | string | must equal the file's shape |
| `notes` | string | one-sentence description |

## Per-pass keys

### `extract_doc`, whole-document summary extraction
- `text`, representative doc/proposal/ADR snippet.
- `expected`, `{ "status", "priority", "components": [...] }`.
  - `status` ∈ {draft, accepted, done, rejected, deferred}
  - `priority` ∈ {low, medium, high}
  - `components`, 1-4 lowercase component tags.

### `extract_code_unit`, per-function cognify + structural grounding gate (AC#7)
- `symbol`, function name.
- `callees`, structural call-graph edges (function names this symbol calls).
- `claimed_side_effects`, what the extraction claims (`[]` / `["none"]` ⇒ claims none).
- `expected_grounding`, `reject` iff the claim is none-like **and** at least one
  callee is side-effecting (the `kb-synthesis` process stage); otherwise `commit`.

`test_curator_fixtures.c` shapes every `extract_code_unit` line through the production
curator seam, invokes the process-module parity handler, and asserts the verdict
matches `expected_grounding`, so a mislabeled fixture fails the build.

### `bridge`, doc-claim ↔ code-unit `implements` linkage
- `topic`, proposal/topic string.
- `expected_files`, 1-4 source files expected to implement it.
