# Proposal: Config field-descriptor table — collapse the parallel config plumbing

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. The remaining save-side work was later
> [rejected](../rejected/config-field-descriptor-save-residual.md) under the Go-or-rejected policy.

- **State:** DONE — delivered scope archived 2026-07-26.
- **Author:** JBailes (drafted by Claude, 2026-07-20).
- **Method:** written against [`docs/lean-refactor-audit.md`](../../lean-refactor-audit.md) —
  evidence-backed, deletion-biased, no speculative redesign.

## Thesis

The config subsystem is the tree's #1 churn hotspot (`config.c`, `config_save.c`,
`config_fields.c` are top-4 by commits/6mo) because **a flat config field is replicated
across up to six hand-maintained sites, and the one table that could be the shared source
of truth — `config_fields[]` — is not used as one**. The replication has already drifted
into shipped bugs. `config_fields[]` already carries per-field `offset`/`size`/`type`; the
fix is to *drive* the flat-scalar parse, save, and schema-check from it — deleting code
rather than adding it (subject to the eligibility inventory in step 0).

This proposal covers **flat scalar fields only**. Genuinely nested object sections
(`memory.*`, `kb.curator.*`, `guardrails.*`, `autonomy.*`) keep their bespoke parsers
— they are the minority and not the churn driver.

### Already resolved — do NOT re-do

An earlier audit (run against the pre-`src/modules/config/` layout) flagged two more
items that are **already fixed on `testing`**, presumably by the `config-fields-cull`
line of work. Verify-before-proposing caught these:

- **Stray mid-array `{NULL}` terminator** killing ~36 `kb_curator_*` rows — fixed;
  `src/modules/config/config_fields.c:381-386` now documents the exact past bug and the
  rows sit correctly ahead of the single terminator (`config_fields.c:426`).
- **Duplicate `memory_rerank_*` field rows** — gone; a `uniq -d` over the current
  `config_fields[]` keys returns nothing.

Only the structural finding below survives.

## Audit entry

- **Scope:** `src/modules/config/config.h` (the `config_t` struct),
  `src/modules/config/config_fields.c` (`config_fields[]`),
  `src/modules/config/config.c` (`config_schema[]` + `config_set_defaults` + the inline
  parse blocks), `src/modules/config/config_save.c` (the per-field writers).

- **Observation:** adding one flat scalar field is shotgun surgery across up to **six**
  sites: (1) the struct member, (2) a `config_fields[]` row, (3) a `config_schema[]`
  row, (4) an inline `cJSON_GetObjectItemCaseSensitive(root, "key")` parse block, (5) a
  `cJSON_Add*ToObject(root, ...)` writer in `config_save.c`, (6) a default in
  `config_set_defaults`. Confirmed first-hand: adding `subagent_ban_enabled` (the
  sub-agent-ban work) took exactly these edits across four files. The parallel tables
  have measurably drifted (measured with `comm` over the sorted key sets, so both
  directions are real — not a `206 − 136` subtraction):
  - `config_fields[]` = **206** rows; `config_schema[]` = **136** rows; the sets overlap
    by 86, so **120** keys are in `config_fields[]` but not `config_schema[]` **and** **63**
    are in `config_schema[]` but not `config_fields[]`. The 63 are largely nested-object
    *section* keys (`memory`, `compact`, `charter`, `cost_reward`, …) that `config_schema[]`
    types as objects — so `config_schema[]` is **not** redundant with `config_fields[]`; it
    additionally governs the nested-section allowlist and cannot simply be deleted (step 2).
  - **82** inline `cJSON_GetObjectItemCaseSensitive(root, …)` blocks in `config.c` (2,035
    lines) and **78** `cJSON_Add*ToObject(root, …)` writers in `config_save.c` (1,421
    lines) — **fewer than the 206 descriptors**, i.e. only a subset of fields is handled
    inline (the rest flow through nested-section parsers or are already table-driven).
    Those inline blocks are the *scalar* subset this proposal targets; producing the exact
    eligibility inventory (which of the 206 are flat-persisted vs nested / alias /
    parse-only / runtime-only, and whether any inline block does validation, coercion, or
    allocation that `offset`/`size`/`type` alone cannot express) is the **first task** of
    the work, not an assumed 1:1 mapping.
  - The drift is a live bug class, self-documented in the code: `config.c` (the
    `require_aimee_git` parse, ~L1344) records that the field "had a `config_fields[]`
    row, a schema row, a default, a `config_save` writer and **NO parse** — so
    `require_aimee_git: false` never loaded"; the `kb_pdf_*` gates carry the mirror note.

- **Why it matters:** it is the direct cause of the config churn — every feature flag
  touches all six sites, and nothing but code review keeps them in sync. A save-side or
  parse-side omission is not a compile error; it silently **reverts the config value on
  restart**, the highest-severity failure class for a config subsystem, and it recurs
  for every new field.

- **Classification rule (defines "flat" precisely):** a descriptor is **flat** iff it
  maps to a single top-level YAML scalar key of a `config_field_t` scalar `type`
  (`CFG_STRING`/`CFG_BOOL`/`CFG_INT`/`CFG_FLOAT`/`CFG_ECON_TIER`) with no dotted section
  prefix, and its inline parse/save does nothing beyond a typed read/write into
  `cfg + offset` (no cross-field derivation, allocation, or coercion). Everything else —
  nested-object sections, aliases, computed/env-derived defaults — is **out of scope** and
  keeps its bespoke handler. Step 0 of the work is to produce and commit this eligibility
  list so "flat" is a checked property, not a judgment call per new field.

- **Lean action:** make `config_fields[]` the single source of truth **for the flat subset**
  and *derive* its plumbing by iterating it:
  1. **Schema-check** — derive the flat-scalar key→type rows from `config_fields[]` and
     have `config_schema[]` hold **only** the remaining nested-section/object entries (the
     63 keys it uniquely owns). This *shrinks* `config_schema[]`; it does not delete it.
  2. **Flat parse** — one generic loop reads `root[key]` by `type` into `cfg + offset`,
     replacing the flat-eligible inline blocks (a subset of the 82); nested-section
     parsers stay.
  3. **Flat save** — one generic loop writes non-default scalars, replacing the
     flat-eligible writers (a subset of the 78). This needs a per-descriptor **default**;
     to avoid introducing a *new* parallel structure, fold `config_set_defaults` for the
     flat subset into the same descriptor so the default lives once, next to the field.

  Net for the flat subset: one `config_fields[]` row per field instead of up to six edit
  sites, and parse/save drift becomes *structurally impossible* there. Non-flat fields are
  unchanged — the proposal does not claim a whole-subsystem single source of truth.

- **Risk:** two load-bearing subtleties. (1) The save side's "persist only when
  non-default" semantics must be reproduced exactly, which requires the per-descriptor
  default to also drive `config_set_defaults` for the flat subset — otherwise defaults
  live in two places and the "single source" claim is hollow. (2) A `config.c`
  save/load round-trip can pass even if a generic parser and writer **drift together**, so
  it does not by itself prove byte-for-byte on-disk compatibility; the migration must also
  diff generated `aimee.yaml` against golden fixtures for a representative field of each
  `type` (default, explicitly-set-to-default, and non-default), and assert the flat
  eligibility list is exhaustive. Blast radius is the on-disk `aimee.yaml` shape, guarded
  today by `test_config.c` round-trip + `test_config_surface` (146 parsed fields). Scope to
  the flat subset; leave every nested section untouched. Incrementally shippable and
  reversible: land the eligibility list + schema shrink first, then parse, then save.

## Sequencing

0. **Eligibility inventory.** Classify all 206 descriptors as flat vs non-flat per the
   rule above; commit the list (and a test asserting it stays exhaustive). Nothing else
   proceeds until this is checked in — it bounds every later step.
1. Add a `default` to `config_field_t` and route `config_set_defaults` for the flat
   subset through it, so the default lives once next to the field (not a new parallel
   table). Guarded by `test_config.c` (defaults unchanged).
2. Derive the flat-scalar rows of `config_schema[]` from `config_fields[]`; keep the ~63
   nested-section/object entries. `config_schema[]` shrinks, not disappears. Guarded by
   `test_config_surface`.
3. Replace the flat-eligible inline parse blocks with a generic descriptor-driven loop;
   keep the nested-section parsers. Guarded by `test_config.c` round-trip + golden
   `aimee.yaml` fixtures.
4. Replace the flat-eligible writers with a generic loop keyed on the per-descriptor
   default. Guarded by the same round-trip + golden fixtures (default,
   explicitly-set-to-default, non-default per `type`).

Each step deletes more than it adds for the flat subset and is independently verifiable
against the existing tests plus the golden fixtures — the lean-refactor bar.
