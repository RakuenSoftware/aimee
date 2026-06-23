> **STATUS: DONE (2026-06-16).** The buildable core (#1 style graph, #3 component
> join + cross-file dead-rule detection, #4-interim static oracle, #5 pipeline
> driver + degraded #2 rules-doc) shipped to `testing` across PRs #351 (WP-A),
> #352 (WP-B), #353 (WP-C), #354 (WP-D), #355 (WP-E), and this WP-F PR. All
> default-off behind `css_style_graph_enabled`. The two deferred upgrades —
> #2→typed-fact and #4→full rendered oracle — remain follow-on proposals gated
> on their own subsystems. Live migration of a real project is the user-gated
> pilot (one component end-to-end first).

# Proposal: CSS migration assistant — exemplar-guided structural conversion

- **State:** done — **APPROVED at human proposal-gate (JBailes, 2026-06-15) on the
  recurring/subsystem path (§9 case 1)** — build #1/#3 as first-class C
  subsystem + #4-interim + #5; #2-upgrade and #4-full remain deferred follow-on
  proposals. Impl plan: `css-migration-assistant.plan.md`. — rev2. Roundtable
  R1 (architect/mistral) returned NOT
  READY with 1 blocker (circular dep: §5 hard-required the §2 typed-fact
  convention model that §6 sequenced after it) + 3 concerns. All folded in
  rev2: convention model made **degradable** (degraded rules-doc form ships in
  the core; typed-fact form is the deferred upgrade), §3 tailwind.config scoped
  as a bounded read-only resolver input, §1 dead-rule claim tightened to
  intra-stylesheet only, interim static oracle made an explicit deliverable.
  R2 automated panel BLOCKED (background-job queue wedged after the first job;
  unwedging needs a live-server restart — not done unilaterally). R2 run as
  adversarial self-review instead (per the "only mistral reviews reliably;
  verify findings yourself" constraint): two real findings folded — (a)
  `tailwind.config.js` is executable JS, so §3 must static-parse or sandbox,
  not evaluate (security/correctness); (b) the build-vs-one-off / NIH question
  raised to the human gate as §9 (genuine user decision, reshapes the
  proposal).
- **Author:** JBailes
- **Date:** 2026-06-15
- **Scope:** Five capabilities that let aimee assist a large structural CSS
  migration — converting a messy Tailwind + post-processor + ad-hoc component
  codebase into plain, structured CSS modelled on a reference ("exemplar")
  project. The five are **not** equal-cost. A complete, useful migration
  assistant ships from this proposal alone (#1 style graph, #3 join, #2 in its
  degraded rules-document form, #4 as an interim static oracle, #5 pipeline
  glue) with **no new subsystems and no blocking dependency**. Two *upgrades*
  — #2 to typed facts and #4 to a full rendered computed-style oracle — depend
  on subsystems that exist only as proposals today and are explicitly deferred
  to follow-on proposals. The dependency map (§6) is the load-bearing part of
  this proposal.
- **Driving use case:** a user inherited a CSS/component mess they did not
  author and wants aimee to convert it to "standard CSS structured like the
  *fizzy* project." The exemplar codebase is the spec; aimee's job is to
  understand both codebases structurally and bridge one onto the other's
  conventions, with verification that the rendered result is unchanged.

## §0 What already exists (so we don't rebuild it)

- **CSS is already indexed — lexically.** `src/extractors.c` registers
  `css_exts[] = {".css"}` and dispatches via `detect_lang`
  (`src/extractors.c:14,583`). `src/extractors_extra.c:225-283`
  (`css_import_line` / `css_export_line`) scans `@import` targets, `.class {`
  selectors, and `--custom-property` declarations into the generic
  `definition_t` (name/kind/line) store consumed by `index.c`
  (`extract_definitions`, `index.c:696`) and persisted through
  `db2_code_index_file_definitions` (`index.c:803-812`). **There is no
  selector model, specificity, cascade, or declaration capture.** This is the
  foundation #1 upgrades.
- **The typed-fact layer does NOT exist.** It is
  `docs/proposals/pending/typed-fact-knowledge-layer.md` (reviewed R5,
  awaiting human sign-off). #2 below cannot be implemented without it.
- **The evidence/regions layer does NOT exist.** It is
  `docs/proposals/pending/structured-pdf-ingestion-and-evidence-layer.md`
  (`kb_doc_regions` / `kb_doc_assets`, proposal-only). #4 below builds on it.
- **Delegate + roundtable machinery DOES exist** and is reused wholesale by #5
  (no new orchestration surface).
- **Tenancy boundary** (per design intent): indexing/style-graph features are
  server/CLI-indexer side (the `code_index` spine); convention *facts* are
  server-memory side (typed-fact); rendered/computed-style *evidence* is
  KB-side behind `/v1` (the same boundary the document-evidence layer lives
  on). #1/#3 → indexer, #2 → server memory, #4 → KB, #5 → delegate core.

## §1 CSS-aware semantic indexing (the style graph) — *self-contained, buildable now*

Replace the line-at-a-time CSS scanner with a real CSS analyzer that emits a
**style graph**, not just symbol names:

- **Selectors** with parsed components and computed **specificity** `(a,b,c)`.
- **Declarations** (`property: value`) bound to their owning selector/rule,
  including `!important` flags.
- **Rules** grouped under at-rule context (`@media`, `@supports`, cascade
  `@layer`).
- Derived signals the migration actually needs. **Standalone from the graph
  alone:** **specificity conflicts**, **duplicate declarations**,
  intra-stylesheet **shadowed/overridden rules**, and recurring **utility
  clusters** (candidate semantic classes). **Requires the §3 join for
  precision:** truly **dead rules** (selectors no live component matches) —
  §1 alone can only flag intra-stylesheet redundancy, not cross-file
  liveness. The proposal does not overstate §1: cross-file dead-code detection
  is a §3 deliverable, not a §1 one.

**Integration points.** New analyzer module (e.g. `src/css_analyze.{c,h}`)
invoked from the extractor dispatch in `extractors.c` for `.css`/`.scss`. The
generic `definition_t` cannot carry specificity/declaration relations, so this
needs a **new DB2 table** for the style graph (sibling of the code-index
tables, e.g. `css_rules` / `css_declarations` keyed by `file_path`), a write
path alongside `db2_code_index_file_definitions`, and a read surface for
retrieval. Keep the existing lexical `definition_t` emission for backward
compatibility (class names still searchable). Default-off behind a config flag
per the rollout-readiness program.

## §2 Exemplar → convention extraction — *typed-fact form BLOCKED; degraded form ships in the core*

Ingest the reference project and derive its conventions — file-layout rules,
naming scheme (BEM / cascade-layer / utility), token strategy, and the "a
component owns its styles" rule. The convention model is **degradable**, which
is what breaks the circular dependency the architect lens flagged (§5 must not
hard-require this section):

- **Degraded form (ships in the §1/§3 core, no new subsystem).** The
  conventions are extracted into a plain written **rules document**
  (`MIGRATION.md`-style) plus a small machine-checkable rule set the §5 driver
  reads directly. The exemplar is indexed via §1, the human (or a delegate)
  confirms the derived rules, and the rules doc *is* the spec each conversion
  checks against. No typed-fact, no validation gate.
- **Upgraded form (BLOCKED on typed-fact layer).** Promoting those rules to
  **typed facts** — assertions like `(exemplar, naming_convention, "BEM")` and
  `(component_X, should_match, convention_Y)` with provenance, contradiction
  detection, and correctable history — is exactly what
  `typed-fact-knowledge-layer.md` provides (`memory_fact_gate`,
  `entity_registry`, `rel_types`). This upgrade gives the convention model a
  validated, contradiction-free home and machine-queryable provenance.

**This proposal does not authorize building typed-fact;** the upgraded form
consumes it once it clears its own human gate. The degraded form is sufficient
for a correct first migration and has no blocking dependency.

## §3 Component ↔ resolved-style join — *depends on #1*

For each component (TSX/JSX), materialize the styles it **actually resolves
to**: Tailwind utility strings expanded to declarations, CSS-module imports,
CSS-in-JS. This is the cross-language join that turns the conversion from
archaeology into a mechanical mapping. aimee already indexes `.ts`/`.tsx`
(`ts_exts`, `extractors.c:9`); the new work is a resolver that links a
component's `className` usage to the #1 style graph and records the join.

**Integration points.** A resolver pass over indexed components that writes a
`component → resolved-declarations` mapping into the #1 storage. Tailwind
expansion needs the project's `tailwind.config`. To avoid scope creep into
general project-config handling in the indexer (architect concern), this is a
**bounded, read-only resolver input**: the config path is passed explicitly to
the resolver pass and used for the token/utility map only — the indexer does
not grow a general project-configuration model, and absence of the config
degrades the resolver to "unresolved" markers (see §8) rather than failing.
**Security note (self-review):** `tailwind.config.js` is *executable
JavaScript*, not static data — naive evaluation is arbitrary code execution.
The resolver must either (a) statically parse the config object and bail to
"unresolved" on any computed/dynamic config, or (b) evaluate it only inside the
existing delegate sandbox, never in the indexer process. Static-parse-only is
the default. Builds directly on §1; cannot precede it.

## §4 Computed-style equivalence oracle — *BLOCKED on evidence layer + new render capability*

The real correctness signal for a CSS migration is **computed style / box
model before vs. after**, not source text or unit tests. Capture a before
snapshot, let the conversion run, diff the after — stored as **evidence**.

**Two hard dependencies:** (a) the evidence/regions schema
(`kb_doc_regions`/`kb_doc_assets` from the document-evidence proposal) is the
natural home for before/after snapshots and is itself unimplemented; (b) aimee
has **no headless-render integration today** — producing computed styles needs
a browser engine in the pipeline. This is the largest and most speculative of
the five and should be scoped as its own proposal after the evidence layer
lands.

**The interim static oracle IS a deliverable of this proposal** (architect
concern: it must not be assumed to exist elsewhere). It is a static
declaration-set equivalence check built directly on the §1 graph — for each
converted unit, compute the resolved declaration set before and after and diff
them — with **no rendering and no browser**. It is strictly weaker than the
full computed-style oracle (it cannot catch cascade/layout interactions that
only appear when rendered) and §5 must log that limitation rather than imply
full equivalence (no-silent-caps). It is nonetheless enough to de-risk the
bulk conversion and unblocks §5 without the evidence layer.

## §5 Migration pipeline (fan-out + verify) — *reuse, smallest new surface*

Once §1, §3, and the §4 interim static oracle exist, the bulk conversion is a
per-component fan-out: each component is a bounded `aimee delegate` task,
converted against the **§2 convention model in its degraded (rules-document)
form** — *not* the typed-fact form, so §5 has **no dependency on the unbuilt
typed-fact layer** — and verified by the interim static oracle, then
roundtable-checked. This reuses `delegate_run_inline` / `delegate_ensemble` and
the proposal lifecycle wholesale — the only new code is a small driver that
enumerates units and tracks per-unit state. Do **not** fan out before the
convention rules doc + pilot + verification harness exist (pilot one
representative component end-to-end first).

## §6 Dependency map & phasing (the decision)

| Cap | Depends on | Status of dependency | Buildable now? |
|-----|-----------|----------------------|----------------|
| #1 style graph | existing extractor/index spine | exists | **yes — self-contained** |
| #3 component↔style join | #1 | n/a (this proposal) | after #1 |
| #2 convention model — **degraded** (rules doc) | #1 | n/a (this proposal) | after #1 |
| #4 interim static oracle | #1 (declaration-set diff, no render) | n/a (this proposal) | after #1 |
| #5 pipeline | #1, #3, #2-degraded, #4-interim | all in this proposal | after #1/#3 |
| #2 convention model — **upgraded** (typed facts) | typed-fact layer | **proposal-only, unbuilt, awaiting gate** | no |
| #4 full computed-style oracle | evidence layer + headless render | **proposal-only + missing capability** | no |

**Recommended order:** #1 → #3 → #2-degraded (rules doc) → #4-interim (static
oracle) → #5 — a complete, useful migration assistant **with no new
subsystems and no blocking dependency**. Then upgrade #2 to typed facts once
the typed-fact layer lands, and add the full computed-style #4 once the
evidence layer lands. Treat **only** those two upgrades as follow-on proposals
gated on their dependencies — they are not deliverables of this one, but the
core migration assistant does not wait on them.

## §7 Out of scope / non-goals

- Building the typed-fact or evidence-layer subsystems (separate proposals,
  separate gates).
- Auto-applying conversions without the oracle + roundtable verification.
- Replacing the lexical class-name index (kept for backward compatibility).
- A general visual-regression product; the oracle is scoped to computed-style
  equivalence for migration safety.

## §8 Risks

- **Scope creep across five features.** Mitigated by §6 phasing: ship the
  self-contained core first, gate the rest on their dependencies.
- **CSS-in-JS / dynamic class names** resist static resolution (#3). The
  resolver must mark unresolved components rather than silently miss them
  (no-silent-caps).
- **Render oracle cost/complexity** (#4) — deferred behind an interim static
  oracle precisely so the core migration does not block on it. When scoped, the
  full oracle renders *untrusted application/exemplar code* in a browser engine
  — a code-execution/SSRF surface that must run sandboxed, never in a trusted
  process.

## §9 Open question for the human gate — build vs. one-off (self-review, contrarian lens)

The sharpest objection to this proposal: **#1 builds a permanent CSS analyzer
(a new C module + DB2 tables) inside aimee.** That is justified only if one of
these holds, and the proposal should not advance until the user picks:

1. **Recurring / product value.** You will run migrations like this more than
   once, *or* a persistent CSS style-graph has standalone value (ongoing
   dead-CSS detection, specificity-conflict surfacing, design-system audits as
   an aimee feature). Then #1 as a first-class subsystem is warranted.
2. **One-off.** You just want *this one* messy app converted. Then a permanent
   C subsystem is over-engineered: a throwaway driver that has a delegate run
   **existing mature CSS tooling** (PostCSS / stylelint / `ts-morph` for the
   component join) and stores the results is far cheaper and avoids
   reimplementing a CSS parser in C (NIH risk). #5's delegate pipeline + the
   degraded #2 rules doc still apply; #1/#3 collapse into "drive existing
   tools," not "build a style-graph subsystem."

This is a genuine fork that reshapes the whole proposal, and only the user
knows which case they are in. It is the first thing to settle at the approval
gate.
