# Anchored (hashline) editing

## Overview

aimee's file editing historically relied on a `str_replace` model: the caller
supplies an exact `old_string` and a `new_string`, and the server swaps the
first (or every) occurrence. That design forces the model to re-emit content it
already read—whitespace, surrounding context, and all—with no stable identifier
for the lines it wants to change. The resulting failure modes ("old_string not
found", "occurs N times", stale-file wrong-apply) are structural and hit cheap
local/open-weight delegates hardest.

Hashline editing replaces this with **anchored, transactional edits**. Every
line read from a file is stamped with a deterministic composite anchor
(`LINE:HASH`). Edits reference those anchors instead of re-emitting text. The
server verifies freshness, owns all offset arithmetic, and applies batches
atomically. The result is fewer tokens, stronger safety guarantees, and
deterministic success on collision and drift scenarios where `str_replace`
silently mis-applies.

## Anchored reads

`read_file` returns each line prefixed with a composite anchor:

```
LINE:HASH| <line content>
```

- **HASH** is a 2-hex display tag derived from a full FNV-1a-64 digest over the
  line's canonical bytes (trailing CR/LF and a line-1 UTF-8 BOM excluded).
- Each anchored read mints an immutable **snapshot** (`snapshot_id`) that
  records every line's full digest plus the file's dominant line-ending, BOM,
  and trailing-newline shape. Snapshots are TTL/LRU evicted. Concurrent reads
  mint independent snapshots—no clobber.
- `raw: true` returns un-anchored bytes (for grep pipelines, binary sniffing).
- `mode: "outline"` returns a tree-sitter symbol skeleton (signatures +
  anchors, no bodies)—one cheap call to map a large file.

**Example anchored read output:**

```
1:a3| import os
2:7b|
3:c4| def main():
4:0a|     print("hello")
```

## Anchored edits

`edit_file` supports a primary anchored path: pass the `snapshot_id` from a
prior read plus an `edits[]` batch. Each edit specifies:

| Field        | Description                                                   |
|--------------|---------------------------------------------------------------|
| `op`         | One of `replace`, `replace_range`, `insert_after`, `delete_range` |
| `at`         | A `LINE:HASH` anchor (for single-line ops)                    |
| `from` / `to`| `LINE:HASH` anchors delimiting a range (for range ops)       |
| `text`       | The replacement or insertion content                          |

**Example call shape:**

```json
{
  "path": "src/main.py",
  "snapshot_id": "snap_abc123",
  "edits": [
    { "op": "replace", "at": "4:0a", "text": "    print(\"world\")" }
  ]
}
```

### Key properties

- **Ordinal as primary key.** The line ordinal disambiguates identical lines;
  the full digest verifies freshness. Each op is verified against the
  snapshot's recorded digest before any write.
- **All-or-nothing.** The batch is applied atomically—no edit renumbers
  another; the server owns all offset arithmetic.
- **Drift handling.** If the file changed since the snapshot was taken, the
  call returns a structured `stale_anchor` payload with re-anchored context
  and a fresh `snapshot_id`. The model retries without a blind re-read.
- **Dry run.** `dry_run: true` previews the unified diff and structural blast
  radius without writing.
- **Byte preservation.** Unchanged lines are written back verbatim. Only edited
  regions are normalized to the file's dominant terminator. A no-final-newline
  file keeps that property.
- **Display-tag stripping.** If a model echoes the `LINE:HASH| ` display prefix
  into `text`, the server strips it—the tag is display-only.
- **Steering advisory (additive, optional).** On a successful commit, the
  response may include an extra optional string field `advisory` when an op
  rewrote a large span (see Migration). It never changes the edit outcome and is
  absent for ordinary edits; consumers should treat it as an optional field.

## Adjacent tools

The same philosophy of stable references and server-side resolution extends to
several related tools:

| Tool                   | Purpose                                                                                      |
|------------------------|----------------------------------------------------------------------------------------------|
| `read_symbol`          | Fetch just a symbol's definition span, anchored.                                             |
| `edit_symbol`          | Rewrite a whole function or type by name. The server resolves the span and swaps it; overloaded or shadowed names return candidates (never a blind resolve). |
| `grep` (`anchored: true`) | Search hits become ready edit anchors, grouped under a per-file `snapshot=...` header.    |
| `run_tests`            | Returns structured `{passed, exit_code, output}` with framework summary and failures kept; the full log is spillable via `tool_output_get`. |

## Lean websearch

The websearch tools are designed to keep page fetches server-side and return
only query-relevant spans:

- **`web_search`** registers `r1..rN` handles for its results.
- **`web_read(ref, query, span, mode)`** fetches a page once server-side and
  returns only query-relevant spans, cited by id and fenced as untrusted. A
  **mandatory literal leg** guarantees exact needles (API names, error strings,
  versions) appear in the result above a lexical term-overlap leg.
  `span=N` or `mode: "full"` recover dropped spans.

### SSRF-safe egress

All fetches are http/https only. The host is resolved once, the resolved IP is
validated against a private/reserved/link-local/metadata deny-list (IPv4 +
IPv6), and the connection is **pinned** to that IP (no re-resolution) to defeat
DNS rebinding. Redirects are not followed—they are returned to the caller.

> **Note:** The neural-embedder "semantic" ranking leg is deferred. The shipped
> path uses the mandatory literal leg + a lexical term-overlap leg.

## Migration & compatibility

The legacy `old_string` / `new_string` / `replace_all` path on `edit_file` is
**retained as a deprecated fallback**. It has not been removed. Agents that have
not adopted anchored edits continue to work, but will not benefit from collision
safety, drift recovery, or token savings. Every use is logged (`edit_deprecated`)
so removal can be driven by measured usage.

**Recommended migration path:**

1. Switch `read_file` calls to default (anchored) mode. Use `raw: true` only
   when you need un-anchored bytes.
2. Replace `old_string`/`new_string` edits with `snapshot_id` + `edits[]`
   batches referencing `LINE:HASH` anchors.
3. For whole-function or whole-type rewrites, prefer `edit_symbol` over
   hand-built `replace_range`—the server resolves the span, avoiding the
   multi-line anchor fumble observed with smaller models. As a guardrail, an
   anchored `replace_range`/`delete_range` covering **≥ 8 lines** returns an
   `advisory` recommending `edit_symbol`.

**When does `old_string` get removed?** Not on a calendar. It is removed only
when a numeric gate is met — the weakest delegate's whole-function pass@k
recovers to within 10 pp of `str_replace` (via `edit_symbol`), the agentic gate
stays green across ≥ 3 runs, `edit_deprecated` telemetry shows negligible
residual usage, and the request-shape change is documented as a deliberate
breaking change. See `benchmarks/hashline/RESULTS.md` for the full criteria.

## Evaluation & status

### Gating harnesses

Two harnesses gate the transition before `old_string` is removed:

1. **`unit-test-hashline-gate`** — a deterministic CI test proving
   model-independent properties: hashline applies every fixture, stays safe
   under collision and drift where `str_replace` silently mis-applies, and
   reproduces zero already-read bytes.
2. **`tools/hashline_agentic_eval.py`** — a multi-turn agentic eval over a
   realistic mutation corpus generated from real repo files
   (`tools/hashline_corpus_gen.py`): collision-at-scale, deep-indent,
   whole-function rewrites, and drift. It renders the real anchored format and
   feeds real tool errors back so retries are measured.

### Live results

Three delegates (mimo-v2.5-pro, MiniMax-M3, codex) × 3 runs on 19 tasks. Gate
criterion: `pass@k ≥ str_replace` AND net token-negative.

- **Gate passes on all three.** Collision is the decisive, consistent win:
  `str_replace` 33–40% pass@k vs hashline 100%, at ~5–15× fewer tokens.
  Token savings are large and consistent even where `str_replace` also succeeds
  (codex: 62 vs 313 tokens at identical 100% pass@k).
- **Caveat:** On MiniMax-M3, whole-function rewrites regressed (pass@k 87% →
  53%)—the smaller model fumbles multi-line anchored ranges. This is why
  `edit_symbol` (server-resolved span) is the recommended path for whole-symbol
  edits, and why `old_string` removal (P5) waits.

### Merge status

Merged to the `testing` branch across:

| PR   | Content                          |
|------|----------------------------------|
| #1396| Edit core + websearch            |
| #1400| Deterministic gate               |
| #1405| Format hardening + fair harness  |
| #1410| Realistic corpus                 |

`old_string` is retained pending whole-function handling via `edit_symbol`.
