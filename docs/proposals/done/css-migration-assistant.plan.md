> **STATUS: DONE (2026-06-16).** The buildable core (#1 style graph, #3 component
> join + cross-file dead-rule detection, #4-interim static oracle, #5 pipeline
> driver + degraded #2 rules-doc) shipped to `testing` across PRs #351 (WP-A),
> #352 (WP-B), #353 (WP-C), #354 (WP-D), #355 (WP-E), and this WP-F PR. All
> default-off behind `css_style_graph_enabled`. The two deferred upgrades —
> #2→typed-fact and #4→full rendered oracle — remain follow-on proposals gated
> on their own subsystems. Live migration of a real project is the user-gated
> pilot (one component end-to-end first).

# Impl plan: CSS migration assistant (approved subsystem path)

> **Plan-roundtable status:** automated panel queue-blocked (delegate worker
> wedged after the first job — same as the proposal R2). Plan converged via
> adversarial self-review; 4 findings folded: WP-A CSS-parser scope + SCSS
> excluded (compiled-CSS only); WP-D Tailwind resolved via the project's real
> sandboxed build, not reimplemented; WP-E gated on resolver coverage; WP-F
> per-unit worktree isolation. Awaiting a healthy worker to (a) optionally get
> a 2nd-model plan review and (b) begin delegate implementation of WP-A.


Plan for `css-migration-assistant.md`, **approved at the human proposal-gate
(JBailes, 2026-06-15) on the recurring/subsystem path**. Scope of this plan:
the buildable core — **#1** style graph, **#3** component↔style join,
**#4-interim** static oracle, **#5** pipeline. The **#2 typed-fact upgrade** and
**#4 full rendered oracle** are deferred follow-on proposals and are NOT in this
plan; #2 appears here only in its **degraded rules-document** form.

Each work packet (WP) is a bounded, delegate-sized unit, dependency-ordered,
each its own PR onto a `feat/css-migration-*` branch → `testing` (never `main`
directly — `main` requires `allow-only-testing-source`). Default-off behind a
config flag per the rollout-readiness program.

## Grounded integration points (verified in tree)

- Extractor dispatch: `src/extractors.c` `detect_lang` (`:583`),
  `index_has_extractor` (`:35`), existing CSS lexical scan
  `src/extractors_extra.c:225-283`.
- Indexer write path: `src/index.c` `extract_definitions` (`:696`) →
  `db2_code_index_file_definitions` (`:803`).
- Code-index schema pattern: `src/db2/schema.sql` (`files`, `file_exports`,
  `terms`, all `… REFERENCES files(id) ON DELETE CASCADE`), applied idempotently
  by `db_apply_schema_postgres` from `src/db2/db2_init.c:232`.
- Code-index ops pattern to mirror: `src/db2/code_index.c` /
  `code_index_ops.c`.
- Config-flag pattern: struct field in `src/headers/config.h`, descriptor in
  `src/config_fields.c` (`{"name", offsetof(...), sizeof(int), DEFAULT,
  CFG_BOOL}`), persistence in `src/config_save.c`.

## WP-A — CSS analyzer core (#1, leaf, no DB)

- New `src/css_analyze.{c,h}`: parse CSS text → **rules** (selector, computed
  **specificity** `(a,b,c)`, at-rule context `@media`/`@supports`/`@layer`,
  source line) + **declarations** (property, value, `!important`). Pure
  functions, **depends only on libc** — keeps it a leaf module (no
  build-integrity layer violation).
- **Scope guard (self-review): a correct CSS selector/specificity parser is the
  single biggest risk in this plan, not a trivial leaf.** WP-A targets a
  *defined subset* and is explicit about the hard cases: `:where()` contributes
  0 specificity, `:is()`/`:not()` take the max of their arguments, pseudo-element
  vs. pseudo-class weighting, attribute selectors, combinators. Anything outside
  the handled subset is recorded as a rule with a **`specificity_uncertain`
  flag** rather than guessed — downstream signals must treat uncertain rows
  conservatively (no silent wrong specificity).
- **SCSS is out of WP-A scope.** `.scss` is a superset (nesting, `@mixin`,
  variables) whose final declarations/specificity cannot be computed without
  compiling it. WP-A handles **plain CSS only**; SCSS projects are indexed from
  their **compiled CSS output** (the post-processor already runs in the
  project's build), not from `.scss` source. Update the dispatch in WP-C
  accordingly — do not silently feed `.scss` into the plain-CSS analyzer.
- `src/tests/test_css_analyze.c`: specificity math (id/class/type, `*`, combinators),
  nested at-rules, `!important`, and **malformed-input resilience** (never
  crash/over-read; truncate gracefully — the existing scanners use fixed
  buffers, match that discipline).
- Gate: `make -j1` (parallel-LTO flakes on this host), clang-format-19.

## WP-B — Style-graph storage (#1) — depends on WP-A

- `src/db2/schema.sql`: add (idempotent `CREATE TABLE IF NOT EXISTS`)
  - `css_rules (id, file_id BIGINT REFERENCES files(id) ON DELETE CASCADE,
    selector TEXT, spec_a/spec_b/spec_c BIGINT, at_context TEXT, line BIGINT)`
  - `css_declarations (id, rule_id BIGINT REFERENCES css_rules(id) ON DELETE
    CASCADE, property TEXT, value TEXT, important BIGINT)`
- New `src/db2/css_graph.{c,h}` mirroring `code_index.c`:
  `db2_css_graph_upsert_file(project, file_path, rules, n)` (delete-then-insert
  per file, same as the code-index file refresh) + query helpers
  (`db2_css_graph_rules_by_selector`, `_declarations_by_property`).
- `src/tests/test_css_graph.c`: round-trip upsert/query via `db2_test_shim`.
- Gate: schema applies cleanly on a fresh DB (db2_init) and on legacy DBs
  (additive only — no ALTER of existing tables).

## WP-C — Wire analyzer into the indexer (#1) — depends on WP-A, WP-B

- `src/index.c` (+ dispatch in `extractors.c`): when `detect_lang(ext) == CSS`,
  call `css_analyze` and `db2_css_graph_upsert_file`. **Keep the existing
  `css_export_line` lexical emission** (class names stay searchable — backward
  compat, per §7).
- New default-off flag `css_style_graph_enabled` (config.h + config_fields.c +
  config_save.c, DEFAULT 0) gating the new write path.
- Derived-signal query helpers (graph-only, no #3): specificity conflicts,
  duplicate declarations, intra-stylesheet shadowed/overridden rules.
- Optional CLI/`/v1` surface `aimee css graph <file>`: **if added**, regen
  `api/openapi-server-v1.yaml` + `gen-cli-v1-routes.py`, satisfy the /v1
  coverage baseline, and STUB the handler in any test that links `server.o`
  (per CI-link gotcha). If not needed for the pipeline, omit to reduce surface.
- Gate: full `aimee git verify` + CI green (build-integrity, clang-format-19,
  /v1 coverage if a route was added).

## WP-D — Component ↔ resolved-style join (#3) — depends on WP-A..C

- Resolver pass linking indexed TSX/TS `className` usage → `css_rules`.
- **Tailwind resolution (self-review — major correction):** do **not**
  reimplement Tailwind's utility→declaration expansion. The utility set is
  generated, version-dependent (v3 vs. v4 differ fundamentally — v4 is
  CSS-first), and not enumerable from `tailwind.config` alone. Instead, **run
  the project's real Tailwind build inside the delegate sandbox** to emit the
  generated CSS, index that output via WP-A..C, and resolve `className` tokens
  against the *generated* `css_rules`. `tailwind.config` is then only
  **static-parsed for human-readable token names** (never JS-evaluated in the
  indexer — §3 security note); absence/dynamic config → "unresolved".
- New `css_component_styles (component_file_id, rule_id/declaration refs,
  resolved BIGINT)`.
- **Mark unresolved components explicitly** (CSS-in-JS / dynamic classes) — no
  silent misses (§8). Enables cross-file dead-rule detection (selectors no live
  component matches), which §1 alone cannot do.
- Tests: utility-string expansion, module import resolution, unresolved marking.

## WP-E — Interim static oracle (#4-interim) — depends on WP-A..D

- New `src/css_oracle.{c,h}`: for a unit, compute the **resolved declaration
  set** before and after a candidate conversion and diff them; return
  equivalence + an explicit **limitation banner** (static only — cannot catch
  cascade/layout interactions that need rendering; §4/§8, no-silent-caps).
- Tests: equivalent vs. divergent declaration sets, reordering, shorthand
  expansion edge cases.
- **Coverage gate (self-review): the oracle is only as strong as WP-D's
  resolver coverage.** If a unit has unresolved (`specificity_uncertain` or
  CSS-in-JS) inputs, the before-set is incomplete and equivalence is not
  guaranteed. WP-F must read the per-unit coverage and **refuse to auto-accept
  conversions below a coverage threshold** (route those to human/roundtable
  review), rather than reporting false "equivalent".

## WP-F — Migration pipeline driver (#5) — depends on WP-D, WP-E + degraded #2

- **Degraded #2 first:** derive a convention **rules document** from the
  exemplar (indexed via WP-A..C) — file layout, naming scheme, token strategy,
  "component owns its styles". A delegate drafts it; the human confirms. This
  is the spec each conversion checks against (no typed-fact dependency).
- Driver: enumerate components → per-unit bounded `aimee delegate` conversion
  against the rules doc → verify via WP-E oracle → roundtable-check → track
  per-unit state. **Reuses `delegate_run_inline` / `delegate_ensemble`
  wholesale** — only the enumerate-and-track driver is new.
- **Per-unit isolation (self-review): parallel conversion delegates mutate
  files and will clobber each other.** Each unit runs in its own git
  worktree/branch (the existing `--worktree` delegate option), converted +
  oracle-verified in isolation, then merged serially. No shared-tree fan-out.
- **Pilot one representative component end-to-end before any fan-out** (§5).

## Sequencing & gates

1. WP-A → WP-B → WP-C = **#1 shippable** (style graph live, default-off).
2. WP-D = **#3**. 3. WP-E = **#4-interim**. 4. WP-F = **#5** (+ degraded #2).

Cross-cutting, every PR: `make -j1` locally (LTO flake), `aimee git verify`,
full GitHub CI green before merge (**never admin-merge through red**); PRs land
on `testing`, promoted to `main` only from `testing`. Delegates implement;
each WP is roundtable-reviewed before merge, with adversarial self-verification
of every finding (only mistral reviews reliably on this panel).
