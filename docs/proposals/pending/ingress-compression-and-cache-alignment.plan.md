# Implementation plan — ingress-compression P0: the Envelope IR

Plan for [ingress-compression-and-cache-alignment.md](ingress-compression-and-cache-alignment.md),
**scoped to P0 only** per the proposal's §7 phasing ("Envelope IR (§1.1) —
refactor `ingress_preinject_build()` to assemble a typed entry list and render
from it. No behavior change, no flag; pure enablement. Blocks everything else.").
Grounded in `src/server/ingress_preinject.c` on `origin/testing`.

## Invariant (the whole point of P0)

**Byte-for-byte identical output.** For every input, the `<aimee-context>`
envelope `ingress_preinject_build()` returns after this change must equal the
envelope it returns today — same block ordering, same group headers, same
separators, same budget/omission behavior, same footer, same truncation note,
same confidence tier. No config flag is added; no source, score, or
retrieval-event emit changes. This is a pure refactor that introduces a typed
intermediate representation between *gathering* sources and *rendering* the
string, so later phases (P1a byte-equivalent fold, P1b lossy fold, …) have
something typed to dispatch a compressor over instead of re-parsing an opaque
rendered string.

## What exists today (the thing being refactored)

`ingress_preinject_build()` (`ingress_preinject.c:343`) interleaves four
concerns into one `dstr_t block`:

1. **Gather** — `kb_client_index_code_search` → `hits[6]`,
   `kb_client_memory_diagnose` → `mems[5]`, `kb_client_memory_facts` → string,
   `ingress_preinject_read_audit_context` → string.
2. **Render** — for each source group it formats per-record *candidates*
   (`format_code_candidate`, `format_memory_preview_candidate`, inline facts /
   audit blocks), appends each via `append_candidate()` (budget gate +
   `omitted_count`), with a group header written on the first candidate that
   actually fits and a single `"\n"` separator before any non-empty later group.
3. **Score** — a confidence score derived from hit/preview counts.
4. **Emit** — the auditable-correctness `retrieval_event` (reads `hits[]`/`mems[]`
   directly), default-off.
5. **Footer** — `context-budget: …` line + `... (N more available …)` note,
   each appended only if it fits `block_budget`.

P0 splits **(2) Render** out behind a typed entry list. **(1), (3), (4) are
untouched** — they keep reading the same `hits[]`/`mems[]`/strings.

## Design

### The IR (new, in `ingress_preinject.h`)

```c
typedef enum {
   ING_SRC_CODE,    /* a code_search hit            */
   ING_SRC_MEMORY,  /* a memory preview             */
   ING_SRC_FACTS,   /* the typed-facts block        */
   ING_SRC_AUDIT,   /* the audit-context block      */
} ingress_source_kind_t;

/* Reserved per proposal §6.5 B2 — one value per lossiness class. P0 sets every
 * entry to ING_XF_NONE (no folding yet); P1a/P1b populate the rest. Declared now
 * so the IR is the contract a compressor dispatches over. */
typedef enum {
   ING_XF_NONE = 0,
   ING_XF_CODE_WHITESPACE_COLLAPSE,
   ING_XF_CODE_COMMENT_STRIP,
   ING_XF_CODE_SIGNATURE_SPAN,
   ING_XF_JSON_FOLD,
} ingress_transform_t;

typedef struct {
   ingress_source_kind_t kind;
   ingress_transform_t   transform;  /* P0: always ING_XF_NONE */
   const char           *header;     /* group header, e.g. "recommended (code):\n";
                                      * emitted once, on the first entry of a group
                                      * that fits the budget. "" for single-entry
                                      * groups that bake their header into preview. */
   char                 *preview;    /* malloc'd per-record body the renderer frees */
} ingress_entry_t;
```

P0 deliberately carries **only** the fields the renderer reads (`kind`,
`header`, `preview`) plus `transform` (the reserved IR contract). The richer
§1.1 fields (`record_id`/`handle`, `sensitivity`/`scope`, `original_ref`,
`budget`, `metrics`) are **not** added in P0 — they would be inert here and are
introduced by the phase that first consumes them (P1b telemetry, P2e handle
store), avoiding the "looks reviewed but unread" field risk (proposal §8).

**Deviation from §1.1's enum** noted for review: the proposal lists
`code_hit | memory_block | audit | (future) tool_result`. Today's envelope also
emits a **typed-facts** group, so P0's enum includes `ING_SRC_FACTS` to model an
existing source. `tool_result` is not added until it has a real producer (§1.2
defers the JSON folder).

### The renderer (new, pure, unit-testable)

```c
/* Render a typed entry list into the pre-envelope block string, applying the
 * existing budget gate, group headers, separators, footer, and truncation note.
 * Pure: no kb, no config, no globals. Frees nothing it does not own. Returns a
 * malloc'd block (possibly empty -> caller treats "" as no-injection). */
char *ingress_render_block(const ingress_entry_t *entries, int count,
                           size_t envelope_budget, int headline_missing_count,
                           int *omitted_count_out);
```

Algorithm (reproduces today's bytes exactly):

```
block_budget = envelope_budget - INGRESS_FOOTER_RESERVE_BYTES   /* same as today */
prev_kind = sentinel; group_first = 1; omitted = 0
for each entry e:
   if e.kind != prev_kind:                 /* group boundary */
      if dstr_len(block) > 0: append "\n"  /* == today's `if (block.len) "\n"` */
      group_first = 1; prev_kind = e.kind
   candidate = (group_first ? e.header : "") + e.preview
   if dstr_len(block) + strlen(candidate) <= block_budget:
      append candidate; group_first = 0    /* header rides the first fitting entry */
   else:
      omitted++                            /* == today's append_candidate miss */
   free per-iteration scratch
if dstr_len(block) > 0:
   append footer (context-budget: …) if it fits block_budget   /* identical text */
   if omitted > 0: append "... (N more …)" note if it fits      /* identical text */
*omitted_count_out = omitted
return steal(block)
```

This is byte-equivalent because:
- The group separator `"\n"` fires on the first entry of each new `kind` when
  the block is non-empty — exactly today's per-group `if (block.len) "\n"` (the
  groups are emitted in the same order: code, memory, facts, audit; an entry
  exists for a group iff that source produced content, so the separator fires on
  exactly the same turns).
- The header rides the **first entry that fits** (`group_first` flips only on a
  successful append) — exactly today's `wrote_header` semantics.
- `append_candidate`'s budget test, `omitted_count`, and the footer/trunc text
  are copied verbatim.

### `ingress_preinject_build()` after P0

1. Gather sources into `hits[]`/`mems[]`/`facts`/`audit` (**unchanged**).
2. Build `ingress_entry_t entries[CAP]` from them:
   - per code hit: `{CODE, NONE, "recommended (code):\n", format_code_body(hit)}`
   - per memory preview: `{MEMORY, NONE, "recommended (memory previews):\n",
     format_memory_body(diag, &headline_missing)}`
   - facts (if present): `{FACTS, NONE, "", "## Known facts\n<facts>\n"}`
   - audit (if present): `{AUDIT, NONE, "", "recommended (audit context):\n<audit>\n"}`
   The existing `format_code_candidate` / `format_memory_preview_candidate`
   become `..._body` helpers that **omit** the header (header moves into the
   entry), keeping their escaping/snippet/whitespace logic byte-identical.
3. Compute `score` from the same counts (**unchanged** — stays in `build`).
4. Emit the `retrieval_event` from `hits[]`/`mems[]` (**unchanged**).
5. `block = ingress_render_block(entries, k, envelope_budget,
   headline_missing_count, &omitted)`; free each `entry.preview`.
6. Wrap: `ingress_preinject_format_envelope(block,
   ingress_preinject_confidence(score))` (**unchanged**).

`CAP` = 6 (code) + 5 (memory) + 1 (facts) + 1 (audit) = 13; a fixed stack array,
no heap list. `score`/footer/headline counting stay where they are.

## File-size & wiring

- `ingress_preinject.c` is 580 lines; the net change is roughly neutral (header
  text moves from candidate formatters into entry construction; the budget/footer
  loop moves into `ingress_render_block`). Comfortably under the 2000-line cap.
- New symbols (`ingress_source_kind_t`, `ingress_transform_t`, `ingress_entry_t`,
  `ingress_render_block`) go in the existing `ingress_preinject.h`. No Makefile
  SRCS change (same `.c`/`.o`). The unit test links the existing
  `ingress_preinject.o` (already built); add the new test target to
  `src/tests/Rules.mk` if a dedicated test file is used.

## Tests

Extend the existing pure-helper tests (the suite that already covers
`ingress_preinject_format_code_block` / `_format_envelope` with no kb dep):

1. **Renderer golden** — hand-built `entries[]` covering all four kinds in order;
   assert the exact block string (headers, `"\n"` separators, footer, trunc note).
2. **Header-on-first-fit** — a code group whose first entry is omitted by a tight
   budget; assert the header rides the second (fitting) entry, and `omitted==1`.
3. **Group-separator suppression** — when an earlier group appended nothing
   (everything omitted), assert no stray `"\n"` precedes the next group (matches
   today's `if (block.len)` guard).
4. **Empty list** → empty string (no footer, no envelope).
5. **Budget edge** — footer/trunc appended only when they fit `block_budget`.
6. **Equivalence smoke** — a representative entry set reproduces a known envelope
   captured from the current code (golden string pinned in the test).

Existing `ingress_preinject` tests must pass unchanged (they exercise the public
pure helpers, which keep their signatures).

## Plan-review resolutions (R1 — roundtable 2026-06-21)

The first plan-gate raised six items; all are resolved in the design above and
verified by the implementation's passing byte-equivalence golden:

1. **FACTS/AUDIT header rule (blocking).** `ING_SRC_FACTS` and `ING_SRC_AUDIT`
   entries set `header = ""` and bake their heading into `preview`
   (`"## Known facts\n<facts>\n"`, `"recommended (audit context):\n<audit>\n"`),
   exactly as today emitted them as a single candidate. Only `ING_SRC_CODE` /
   `ING_SRC_MEMORY` carry a separate group `header` (written once, on the first
   fitting entry).
2. **Separator on a fully-omitted prior group (blocking).** The `"\n"` is
   appended at a group boundary **only when the block is already non-empty**
   (`if dstr_len(block) > 0`). If an earlier group appended nothing, the block is
   still empty, so no separator is emitted — byte-identical to today's
   `if (block.len) "\n"`. Covered by the "group-separator suppression" test.
3. **`headline_missing_count` (blocking).** It is computed during memory-entry
   construction and threaded into `ingress_render_block(...,
   headline_missing_count, ...)`, which renders it verbatim in the footer — part
   of the golden assertion (`headline_missing_count=1`).
4. **`explore-with` line (blocking).** This is **not** the block renderer's
   concern: `ingress_render_block` produces the *pre-envelope block* (groups +
   footer); the unchanged `ingress_preinject_format_envelope()` wraps it with
   `<aimee-context …>`, the `explore-with:` line, and `</aimee-context>`. The
   golden confirms the line is present and unchanged.
5. **`ingress_transform_t` unused in P0 (suggestion).** Documented in the enum:
   P0 sets every entry to `ING_XF_NONE`; the enum is the reserved IR contract for
   P1a/P1b folds.
6. **Fate of `format_*` callers (nit).** `format_code_candidate` /
   `format_memory_preview_candidate` are renamed to header-less `_body` helpers
   and have a single caller (entry construction) — no dead code.

## Risks / rollback

- **Sole risk is a byte drift** vs today. Mitigated by the golden tests above and
  by leaving gather/score/emit untouched. There is no flag and no behavior toggle
  to get wrong.
- Rollback is a straight revert (single commit, one `.c`/`.h` + test).
- No schema, no config, no wire-shape, no provider-API dependency in P0.

## Out of scope (later phases, separate plans)

P1a byte-equivalent code fold + `ingress_compress_enabled`; P1b lossy folds +
resolvers/telemetry; P2/P2e durable reachability + handle store; P3 cache-prefix
placement; P4 failure mining; P5 Anthropic ingress. P0 only lays the IR.
