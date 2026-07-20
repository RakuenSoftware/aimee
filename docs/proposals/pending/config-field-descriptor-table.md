# Proposal: Config field-descriptor table — collapse the parallel config plumbing

- **State:** proposed (pending — not started).
- **Author:** JBailes (drafted by Claude, 2026-07-20).
- **Method:** written against [`docs/lean-refactor-audit.md`](../../lean-refactor-audit.md) —
  evidence-backed, deletion-biased, no speculative redesign.

## Thesis

The config subsystem is the tree's #1 churn hotspot (`config.c`, `config_save.c`,
`config_fields.c` are top-4 by commits/6mo) because **every flat config field is
replicated across up to six hand-maintained sites with no shared source of truth**,
and the replication has already drifted into shipped bugs. `config_fields[]` already
carries enough per-field metadata (`offset`, `size`, `type`) to *drive* the parse,
save, and schema-check generically — so the fix deletes code rather than adding it.

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
  have measurably drifted:
  - `config_fields[]` = **206** rows; `config_schema[]` = **136** rows; **120** keys in
    `config_fields[]` are absent from `config_schema[]`.
  - **82** inline `cJSON_GetObjectItemCaseSensitive(root, …)` parse blocks in `config.c`
    (2,035 lines); **78** `cJSON_Add*ToObject(root, …)` writers in `config_save.c`
    (1,421 lines) — most one-per-scalar mechanical mirrors.
  - The drift is a live bug class, self-documented in the code: `config.c` (the
    `require_aimee_git` parse, ~L1344) records that the field "had a `config_fields[]`
    row, a schema row, a default, a `config_save` writer and **NO parse** — so
    `require_aimee_git: false` never loaded"; the `kb_pdf_*` gates carry the mirror note.

- **Why it matters:** it is the direct cause of the config churn — every feature flag
  touches all six sites, and nothing but code review keeps them in sync. A save-side or
  parse-side omission is not a compile error; it silently **reverts the config value on
  restart**, the highest-severity failure class for a config subsystem, and it recurs
  for every new field.

- **Lean action:** make `config_fields[]` the single source of truth for flat scalars
  and *derive* the rest by iterating it:
  1. **Schema-check** — drop the separate `config_schema[]`/`config_schema.inc`; derive
     the key→type allowlist from `config_fields[]` (which already has `type`).
  2. **Flat parse** — one generic loop reads `root[key]` by `type` into `cfg + offset`,
     replacing the ~82 inline scalar blocks.
  3. **Flat save** — one generic loop writes non-default scalars, replacing the ~78
     writers. This needs a per-descriptor **default** (add a `default` to
     `config_field_t`, or a parallel defaults descriptor) so the current "persist only
     when non-default" semantics reproduce exactly.

  Net: one `config_fields[]` row per flat field instead of up to six edit sites, and
  parse/save drift becomes *structurally impossible* for scalars.

- **Risk:** the load-bearing subtlety is the save side's per-field "persist only when
  non-default" behavior — today bespoke per field; the table-driven version must carry a
  per-descriptor default to match it byte-for-byte on the wire. Blast radius is the
  on-disk `aimee.yaml` shape, which is guarded by the `test_config.c` save/load
  round-trip and `test_config_surface` (146 parsed fields). Scope to flat scalars; leave
  every nested object section untouched. The work is incrementally shippable and
  reversible: land schema-derivation first, then parse, then save — each behind the
  existing round-trip tests, no new harness required.

## Sequencing

1. Add a `default` column (or defaults descriptor) to `config_field_t`; populate it for
   flat scalars only. No behavior change — pure data.
2. Replace `config_schema[]` type lookups with a derivation over `config_fields[]`;
   delete `config_schema.inc`. Guarded by `test_config_surface`.
3. Replace the ~82 inline scalar parse blocks with a generic descriptor-driven loop;
   keep the nested-section parsers. Guarded by `test_config.c` round-trip.
4. Replace the ~78 scalar writers with a generic loop keyed on the per-descriptor
   default. Guarded by `test_config.c` round-trip.

Each step deletes more than it adds and is independently verifiable against the existing
tests — the lean-refactor bar.
