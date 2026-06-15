# Proposal: typed-fact knowledge layer for identity & world facts

- **State:** draft - reviewed, converged at R3 (reviewer / architect / security
  lenses across minimax + mistral all returned no major issues)
- **Author:** JBailes
- **Date:** 2026-06-12
- **Charter roles:** Extract (pattern-first + typed-triple extraction), Gate
  (write-time type validation), Recall (typed retrieval + prose injection),
  Calibrate (provenance-keyed confidence classes), Curate (self-extending
  ontology promotion), Gate-Promote (default-off flag rollout per the
  readiness program).
- **Scope:** a new ontology subsystem (`src/db2/rel_types.{c,h}` +
  migration), an entity-identity registry (`src/db2/entity_registry.{c,h}` +
  alias/conflict tables), a typed-fact write gate
  (`src/memory_fact_gate.{c,h}`, sibling of `memory_gate_check`), extensions
  to the existing typed-edge store (`src/db2/entity_edges.{c,h}` already
  carries `relation_id`/`subject_kind`/`object_kind`), a pattern-first
  extractor (`src/memory_extract_patterns.{c,h}`), a correction/retraction
  path (`src/memory_correction.{c,h}` + ingress hook in
  `src/server/ingress_preinject.c`), retrieval + prose injection on the recall
  path, a typed `/v1/facts/*` surface (`server_http_routes.inc` + handler),
  `aimee fact` / `aimee expand` CLI, and config plumbing
  (`src/headers/config.h`, `src/config_fields.c`, `src/config_sections.c`,
  `src/config_save.c`). Unit + integration tests. No new service, no new model.

## Goal

Give aimee a **structured, write-validated layer for identity and world facts**
— people, places, devices, IPs, hostnames, preferences, relationships — that
sits *alongside* the existing code-knowledge KB rather than replacing any of it.

Today aimee stores memories as free-text `content` (`memory_t`) plus a
**co-occurrence** entity graph (`entity_edges` with `co_edited` / `co_discussed`
relations). That is excellent for code and episodic recall, but it cannot give
*precise, contradiction-free* answers to factual questions like "what is the
NAS's IP" or "who is the user's spouse." Free-text + cosine similarity conflates
"the user lives in Toronto" with "Vancouver is in Canada"; a co-occurrence edge
records *that* two terms appeared together, not the *typed relationship* between
them. The result is that personal/world facts are recalled by vibe, not by
assertion.

This proposal adds seven capabilities that turn those facts into **typed,
validated propositions** with explicit provenance, correctable history, and a
self-extending schema — while reusing aimee's existing gate, lifecycle, graph,
conflict, and maintenance machinery wherever it already exists.

## §0 What already exists (so we don't rebuild it)

- **The typed-edge store already has the columns.** `db2_entity_edge_upsert`
  records `relation_id`, `subject_kind`, `object_kind` on fresh inserts, and
  `db2_entity_edge_walk_step_typed` reads them back (defaulting legacy rows to
  `REL_CO_DISCUSSED` / `NODE_OTHER`). The typed-fact layer **populates these
  columns with semantic relations** instead of co-occurrence defaults; it does
  not invent a second graph. **It does, however, share one physical table with
  co-occurrence rows, so the two row populations must be explicitly separable —
  see the `edge_class` discriminator below (R1-A1).**
- **A write gate already exists.** `memory_gate_check` returns a
  `gate_verdict_t` (incl. `GATE_DOWNGRADE`). The fact gate is a sibling that
  validates *triples against an ontology*, not the prose-memory gate; both share
  the verdict idiom.
- **Lifecycle, confidence, promote/demote, and expiry already exist.**
  `kind_lifecycle_t` carries `promote_confidence` / `demote_confidence`;
  `memory_run_maintenance(promoted, demoted, expired)` already sweeps. The
  confidence-class model (§5) is a **policy layered on these primitives**, not a
  new scheduler.
- **Conflict detection and soft-delete already exist.** `memory_conflict.c`
  detects contradictions; the project rule is *always retain the origin
  artifact* (archive, never hard-delete). The correction path (§4) adds a
  *declarative per-relationship* policy on top of this, not a new deletion
  engine.
- **Lineage/provenance already exists.** `memory_lineage_insert(object_type,
  object_id, source_kind, source_ref, confidence)` with `source_kind` of
  `session` / `cognify` / `extract` / `import`, and `provenance_category` on
  `memory_t`. Confidence classes (§5) bind decay policy to these existing
  provenance signals.
- **A background curation loop already exists.** `memory_maintenance.c`
  (replay / compact / prune / summarize) is the host for ontology promotion
  (§2) and class expiry (§5) — new modes, not a new daemon.
- **Context pre-injection already exists.** `ingress_preinject_*` builds the
  `<aimee-context confidence=…>` envelope. Typed facts join this envelope as a
  new section (§7); we do not add a second injection point.
- **A curator/extraction surface already exists** (`kb_curator_*`,
  `memory_scan.c`). Pattern-first extraction (§6) and triple rewrite reuse the
  curator plumbing rather than standing up a parallel pipeline.

The net new persistent objects are: a `rel_types` ontology table, an
`entity_registry` + `entity_aliases` + `entity_name_conflicts` set, and a few
columns on `entity_edges` (an `edge_class` discriminator, provenance,
confidence-class, superseded_at, valid-time). Everything else is policy over
existing primitives.

**Storage boundary (R1-A1, blocking).** Because typed semantic edges and legacy
`co_edited` / `co_discussed` rows live in the **same** `entity_edges` table, the
"alongside, not merged" claim is only true if the two populations are
distinguishable in storage. A typed-recall walk must never silently include a
co-occurrence edge, and vice-versa. We therefore add an explicit
`edge_class` discriminator column (`semantic` | `cooccurrence`), default
`cooccurrence` for all existing rows (matching today's `REL_CO_DISCUSSED`
default). Every typed-fact write sets `edge_class = 'semantic'`; the typed recall
path (§1) and the prose/graph recall path filter on it. The union happens *only*
at injection (§7), keyed by this column — never by an implicit `relation_id`
heuristic. This replaces the earlier hand-wave that the layers were "unioned at
injection, not merged in storage": they *are* co-located in storage, and
`edge_class` is what keeps them separable.

---

## §1 Typed-relationship ontology + write-time type validation

**Problem.** aimee has no notion of `works_for` / `spouse` / `lives_at` /
`device_has_ip` as *typed* relations with subject/object type constraints. A
fact is just text, so nothing rejects "the printer works_for the kernel."

**Design.** A `rel_types` ontology table is the single source of truth for
relationship semantics. Each row is **self-describing metadata** — no hardcoded
rules:

| column | meaning |
|---|---|
| `rel_type` | canonical name (`works_for`, `device_has_ip`, …) |
| `head_kinds` / `tail_kinds` | allowed subject/object entity kinds (or `ANY` / `SCALAR`) |
| `is_symmetric` | one row implies both directions (`spouse`, `knows`) |
| `inverse_rel_type` | auto-enforced inverse (`parent_of` ↔ `child_of`) |
| `correction_behavior` | `supersede` \| `hard_delete` \| `immutable` (see §4) |
| `category` | grouping (`family`, `network`, `work`, `identity`) |
| `sensitivity` | PII gating tier (see §7) |
| `is_hierarchy_rel` | participates in classification chains |

A new **typed-fact write gate** (`memory_fact_gate`, sibling of
`memory_gate_check`) receives candidate triples and validates them against this
ontology *before commit*: unknown rel_type → stage as low-confidence (§5);
subject/object kind not in `head_kinds`/`tail_kinds` → reject; asymmetric pair
that contradicts its inverse → reject. Validated triples are written to
`entity_edges` with `relation_id` resolving to the rel_type and
`subject_kind`/`object_kind` populated (columns that already exist).

A seed ontology ships in code (the `SEED_ONTOLOGY` idiom) as a DB-unavailable
fallback; the live ontology is read from `rel_types` with a short TTL cache (the
same registry-cache pattern aimee already uses for hot config tables).

**Gate call site (R1-B3, blocking).** `memory_fact_gate` is not a free-floating
validator — it is the *single* commit point for typed edges. All three triple
emitters (the pattern extractor §6, the model rewrite, and the existing
`kb_curator_*` / `memory_scan.c` edge path §0) route their candidate triples
through `memory_fact_gate` *before* `db2_entity_edge_upsert`; no emitter writes a
`semantic` edge directly. The gate is the only code path that sets
`edge_class = 'semantic'`. This removes the "unspecified caller / duplicate-write
from three paths" gap.

**Gate failure modes are distinct (R2-2, refined R3-2).** The ontology has two
sources — the in-code `SEED_ONTOLOGY` (always available) and the live `rel_types`
table (DB) — so "unreachable" is not all-or-nothing:

- **rel_type known (seed *or* live table)** → validate normally against whichever
  source is available. A DB outage does **not** block seed-known types
  (`works_for`, `device_has_ip`, …); they validate against the in-code seed and
  commit. This is the point of shipping the seed.
- **rel_type novel, live ontology reachable** → stage as Class C (§5) for later
  promotion (§2). See "provisional rel_type" below for where it lives.
- **rel_type novel *and* live ontology unreachable** → the gate cannot record the
  occurrence/staging (that needs the `rel_types` / `ontology_evaluations` tables),
  so it **defers** the `semantic` write to a bounded retry queue. On DB recovery
  the queued triples re-enter the gate. Co-occurrence writes are unaffected.

"Fail closed" means *never silently commit an unvalidated novel fact* — seed-known
facts still flow; only genuinely-novel writes during a DB outage are deferred.

**Provisional rel_type for staged Class C (R3-1).** A Class-C triple still needs
`entity_edges.relation_id` to resolve. So staging a novel rel_type inserts a
**provisional `rel_types` row** (`status = provisional`, mirroring the
`entity_registry` provisional pattern, §3); the edge's `relation_id` points at it
immediately. Promotion (§2) flips the row to `active`; rejection/expiry suppresses
it and its Class-C edges. No edge ever carries a dangling `relation_id`.

**Ontology self-validation (R1-D1, blocking).** `rel_types` rows are themselves
validated at load/insert time, not trusted as free text:

- `head_kinds` / `tail_kinds` are constrained to a closed enum of entity kinds
  (`PERSON`, `DEVICE`, `PLACE`, …, plus `ANY` / `SCALAR`); an unknown kind
  (`"PERSONN"`) is rejected at ontology load, not silently allowed to reject all
  facts of that type later. `SCALAR` is a first-class kind so value-typed
  relations (`age = 30`) are expressible.
- `is_symmetric` + `inverse_rel_type` are checked for consistency at load: a
  symmetric type's inverse must be itself; a non-symmetric type's inverse must
  exist and have a matching head/tail flip. Inconsistent rows (`is_symmetric=true`
  with `inverse_rel_type=child_of`) are rejected at ontology load.
- `correction_behavior` is `NOT NULL` with a default of `supersede`; the
  migration backfills existing/seed rows so §4 never sees a NULL policy.
- `rel_type` names are normalized to a single convention (lower `snake_case`,
  case-insensitive lookup) so `worksFor` and `works_for` resolve to one canonical
  type instead of fragmenting the ontology.
- `sensitivity` (R3-4) is a closed enum (`normal` | `pii` | `secret`), `NOT NULL`,
  and **defaults to the restrictive `pii`** for any type whose sensitivity wasn't
  explicitly classified — so a learned/seed-omitted type fails *closed* (withheld
  from injection, §7) rather than leaking. A promoted type (§2) must have its
  sensitivity set at the human-approval step before it can leave `pii`.

**Trust boundary (R1-S1).** The model is treated as untrusted input throughout:
it can be prompted into emitting triples the user never asserted, and it is the
source of its *own* rel_type-promotion verdicts (§2). Model-emitted triples
therefore can never enter at Class A (user authority, §5); the gate caps any
model-sourced write at Class B/C regardless of the model's stated confidence, and
promotion verdicts (§2) require the occurrence threshold *and* a human-approvable
step for anything that changes the ontology shape.

**Recall payoff.** Factual queries traverse typed edges with a 1-hop graph walk
+ bounded hierarchy expansion (both already implemented in `memory_graph.c` /
`memory_episodes` BFS), returning *assertions* ("NAS `device_has_ip`
192.168.1.254"), not nearest-neighbour text.

## §2 Self-extending, metadata-driven ontology + `aimee expand`

**Problem.** The ontology can't grow on its own, and there's no way to teach
aimee a domain's structure before facts arrive.

**Design — promotion pipeline.** Novel rel_types seen by the gate (§1)
increment an `occurrence_count` in an `ontology_evaluations` table. When count
≥ N (default 3), a maintenance-cycle mode asks the model to evaluate: **approve**
(confidence ≥ 0.7 → INSERT into `rel_types`), **map to existing**
(embedding similarity > 0.85 → alias onto the canonical type), or **reject**.
This rides `memory_maintenance.c` and the existing curator/LLM call path; it is
a new mode, not a new loop.

**Design — `aimee expand <domain> [url]`.** A CLI/`/v1` command that seeds the
ontology for a domain *up front*: `aimee expand networking` plants the device /
IP / MAC / hostname rel_types and their type constraints; `aimee expand
kubernetes https://…/docs/concepts/` fetches the doc, extracts the domain's
relationship structure, and proposes rel_types through the same promotion gate
(human-approvable, never auto-trusted). Every learned type remains correctable
(§4). This reuses `kb_client_docs` / curator ingestion for the fetch+extract
step.

## §3 Entity canonicalization — surrogate ids + alias resolution

**Problem.** `entity_edges` keys endpoints by **name string** (`char
node[512]`). "DevBox", "the workstation", and "my main box" fragment into three
unrelated nodes; "Theo" and "Theodore" never reconcile (or wrongly merge).

**Design.** A new `entity_registry` assigns each entity a **canonical surrogate
id** and a kind; an `entity_aliases` table maps display names → canonical id
with one `is_preferred` row per entity (ON CONFLICT preserves the existing
preferred name — never silently downgrades).

**Resolution semantics (R1-B2, blocking — corrected).** The earlier draft made
strict-match the *only* default and punted all variation to an async queue. That
fails the motivating case: in one session a user says "my workstation is DevBox"
then asks "what's my workstation's IP?" — an async-only merge answers "no
relation" because the canonical write hasn't merged yet. So resolution is now
**two-tier and synchronous on the hot path**:

1. **Explicit binding is immediate.** When the user asserts an alias
   (`X is Y`, `call it Z`), the alias row is written synchronously and the
   endpoint resolves on the *same* turn. Identity declared by the user is never
   deferred.
2. **Implicit near-match is synchronous but conservative.** Normalized
   exact-match plus a tight, logged near-match (case/punctuation/whitespace +
   a high-similarity threshold) resolves inline; every near-match merge writes an
   audit row so a wrong merge is reversible (see unmerge below).
3. **Genuinely ambiguous collisions** (multiple plausible canonical targets) are
   the *only* thing queued in `entity_name_conflicts`, and they **block neither**
   the write (it lands under a provisional id) nor recall (it returns with an
   ambiguity flag). The §3 claim is now honest: synchronous resolution handles
   the common case; the async queue is strictly for true ambiguity.

**Registry constraints (R1-D2, blocking).** The schema enforces what the prose
assumed:

- `canonical_id` is a **globally-unique surrogate primary key** (R2-5) — never
  reused across kinds or entities; `kind` is an attribute of the row, not part of
  the identity. (The earlier `UNIQUE (kind, canonical_id)` wording was weaker than
  the "never reused across kinds" prose claimed; the PK is the correct, stronger
  constraint.)
- `entity_registry` carries a **`status`** column — `active` | `provisional` |
  `merged` (R2-4). An ambiguous write (§3 tier 3) lands as a `provisional` row
  (a real schema home, not a floating id); conflict resolution flips it to
  `active` or `merged`. A `merged` row keeps a `merged_into` pointer so recall
  transparently follows it and `unmerge` can reverse it.
- `entity_aliases` resolution is **single-hop** (alias → canonical id only;
  aliases never point at other aliases), which makes circular-alias chains
  (`A→B`, `B→A`) structurally impossible rather than guarded after the fact.
- `entity_aliases` carries its own **`suppressed`** flag (R3-3) so a `hard_delete`
  tombstone (§4) can stop an alias from resolving while retaining the row for
  audit — symmetric with the `suppressed` flag on `entity_edges`. Tombstoning is
  never a literal `DELETE` on either table.
- `entity_name_conflicts` has a defined lifecycle: a `status`
  (`open` / `resolved` / `failed`), priority by occurrence frequency, a bounded
  retry on model-resolution failure, and escalation to a human-visible list when
  retries exhaust — unresolved conflicts never silently vanish.
- **Merge / unmerge are first-class operations**, not implied: a `merge(a, b)`
  collapses two canonical entities (re-pointing aliases and edges, recording the
  merge) and an `unmerge` reverses a recorded merge (including the audited
  near-matches from tier 2). Both are exposed on the `/v1/facts/*` surface and
  covered by tests.

Edge writes resolve both endpoints to canonical ids *upstream* of
`db2_entity_edge_upsert`, so deduplication happens by identity, not by spelling.
Recall attaches all known aliases (with the preferred flag) to each returned
entity. This is the single highest-leverage correctness fix: it removes
name-variation fragmentation across the entire typed layer.

## §4 Declarative correction policy + retraction-signal detection

**Problem.** aimee detects conflicts but has no *per-relationship* correction
contract and no explicit user-driven retraction path ("forget that", "that's
wrong").

**Design.** `correction_behavior` (a `rel_types` column from §1) declares, per
relationship, what a correction *does*:

- **`supersede`** — set `superseded_at = now()` on the old edge, insert the new
  one (default; full audit trail, consistent with *always keep the origin
  artifact*).
- **`hard_delete`** — *tombstone*, not a literal row delete (R2-1): the edge and
  its aliases are removed from **active resolution** (a `suppressed` flag + the
  `superseded_at` stamp) but the row is retained, honouring §0's *always retain
  the origin artifact* rule. Used for `also_known_as` / `pref_name` where a stale
  alias actively misleads and must stop resolving — but stays auditable. The name
  is kept for familiarity; the behaviour is suppress-and-archive, never a literal
  `DELETE`.
- **`immutable`** — reject *model/inferred* corrections to an already-asserted
  value (e.g. `born_on`): a Class B/C source cannot overwrite it. This does **not**
  override the user (R1-B1) — see the authority rule below.

**Retraction-signal detection** runs in the ingress *before* the model answers:
a cheap scan (regex-first, §6) flags "forget / delete / that's wrong / no longer"
and routes a `{subject, rel_type, old_value}` retraction through a new
`/v1/facts/retract` handler applying the declared behavior.

**Authority vs. immutability (R1-B1, blocking — resolved).** `correction_behavior`
governs the **inferred** write path only. A **user** correction always wins:
`immutable` blocks Class B/C (model/inferred) overwrites but a direct user
assertion supersedes the prior value as a new Class A fact (the old value is
superseded, not hard-deleted, preserving history). So the two rules no longer
conflict — `immutable` means "no *model* may silently rewrite this," not "the
user may never change it." A type that should resist even user edits is a
separate, explicitly-flagged case and is out of scope for R1.

**Bitemporal recall (R1-B4, blocking — clarified).** The model is genuinely
bitemporal with two independently-named axes, both queryable:

- **transaction time** — `superseded_at` (when aimee stopped believing the edge);
  `NULL` = currently believed.
- **valid time** — `valid_from` / `valid_to` (the real-world interval the fact
  held); open-ended `valid_to IS NULL` = still holds.

Current-state queries filter `superseded_at IS NULL AND (valid_to IS NULL OR
valid_to > now())`. "What IP did the NAS *used to* have?" is a valid-time query
(`valid_to < now()`); "what did aimee believe last week?" is a transaction-time
query (`superseded_at > last_week OR superseded_at IS NULL`). Corrected edges are
superseded, never dropped, so both axes have data to return.

## §5 Provenance-keyed confidence classes

**Problem.** aimee's `confidence` is a scalar; decay is not bound to *who
asserted the fact*, so a model's speculation and a user's direct statement age
the same way.

**Design.** A thin **class** policy over the existing `confidence` /
`provenance_category` / lineage `source_kind` primitives (§0), stored as a
`confidence_class` column:

| class | source | confidence | lifecycle |
|---|---|---|---|
| **A** | user-stated (`source_kind=session`, direct) | 1.0 | permanent; wins all conflicts |
| **B** | model-inferred, ontology-consistent | 0.6–0.8 | after 3 confirmations → **durable B** (no TTL); still below A |
| **C** | model speculation / novel rel_type | 0.4 | **expire after 30 days unless confirmed** |

**B never becomes A (R2-3).** "Promotion" elevates a Class B fact from
*expiring* to *durable* (TTL removed) — it does **not** make it Class A. A is
reserved for direct user assertion (§1 trust boundary); a model-inferred fact,
however many times re-observed, stays Class B. Conflict/decay semantics of a
durable-B fact are explicit: it loses to any Class A fact on the same
`(subject, rel_type)`, wins over unconfirmed B and all C, and no longer expires.
This removes the undefined "permanent" state the earlier draft implied.

Promotion (B→durable-B at 3 confirmations, never to A) and expiry (C after TTL)
are new **modes of `memory_run_maintenance`**, reusing the existing
`promoted/demoted/expired` counters and `kind_lifecycle` thresholds. The class
maps cleanly onto the memory metadata types already in use (`user` ⇒ A,
`feedback`/`project` ⇒ B once confirmed, transient inferences ⇒ C). The point is
that **unconfirmed model speculation cannot calcify into a remembered "fact"** —
it decays unless reality reinforces it.

## §6 Pattern-first extraction before the model

**Problem.** Every fact extraction today costs a model call. Many facts are
trivially regex-shaped (IPv4/IPv6, MAC, dates, "X is Y", "my <noun> is <value>").

**Design.** A `memory_extract_patterns` pass runs *first* on the ingress text:
high-precision regexes emit candidate triples directly; only the residual text
that the patterns don't cover is escalated to the model rewrite. This is a
direct ingest-cost win and **plugs into the in-flight cost-accounting work**
(see `ingress-cost-accounting-and-optimizations.md` / the cost ledger) — the
saved model calls show up as measured spend reduction. It also lowers latency on
the hot ingress path and doubles as the cheap scan for retraction signals (§4).
Pattern hits are still validated by the §1 gate; regex precision buys cost, not
a bypass of validation.

## §7 Per-attribute PII sensitivity gating

**Problem.** aimee's scope model (`MEMORY_SCOPE_GLOBAL` etc.) is coarse; it
can't express "this attribute is sensitive — don't inject it unless explicitly
asked."

**Design.** The `sensitivity` column on `rel_types` (§1) tags attributes
(`born_on`, `lives_at`, `height`, credentials) as PII. On the recall path, the
pre-injection envelope **withholds sensitive facts unless the current turn
explicitly requests them** (keyword/intent match), while identity facts needed
for normal operation (preferred name, role) always pass at confidence ≥ 0.4.
Because `sensitivity` is `NOT NULL` and defaults to `pii` (§1, R3-4), any type
whose sensitivity was never explicitly classified is withheld by default — the
recall path fails *closed*, so a learned or seed-omitted attribute cannot leak PII
simply because no one tagged it. This is a privacy refinement to
`ingress_preinject_*`, gated behind the same default-off flag as the rest of the
layer.

---

## Data model summary

New/extended db2 objects (one migration, following the `agent_outcomes` table
idiom):

- `rel_types` — ontology (§1): `rel_type` (lower snake_case, case-insensitive
  key), `head_kinds`, `tail_kinds` (closed entity-kind enum incl. `ANY`/`SCALAR`,
  validated at load), `is_symmetric`, `inverse_rel_type` (consistency-checked
  against `is_symmetric` at load), `correction_behavior` (`NOT NULL` default
  `supersede`), `category`, `sensitivity` (closed enum `normal`/`pii`/`secret`,
  `NOT NULL` default `pii`), `is_hierarchy_rel`, `status`
  (`active`/`provisional`; provisional = staged novel type pending promotion §2,
  so Class-C edges have a resolvable `relation_id`).
- `entity_registry` — globally-unique surrogate `canonical_id` PK + `kind` +
  `status` (`active`/`provisional`/`merged`) + `merged_into` pointer (§3).
- `entity_aliases` — display name → canonical id (single-hop, no alias→alias),
  `is_preferred`, `suppressed` (tombstone flag, §4) (§3).
- `entity_name_conflicts` — true-ambiguity queue (§3) with
  `status` (`open`/`resolved`/`failed`), priority, bounded retry + escalation.
- `entity_merges` — audited merge/near-match log enabling unmerge (§3).
- `ontology_evaluations` — novel-rel_type occurrence counter + verdicts (§2).
- `entity_edges` **+columns** — `edge_class` (`semantic`/`cooccurrence`, default
  `cooccurrence`; §0 storage boundary), `confidence_class`, `suppressed`
  (tombstone flag for `hard_delete`, §4), `superseded_at` (transaction time),
  `valid_from` / `valid_to` (valid time, §4), provenance link (`relation_id` /
  `subject_kind` / `object_kind` already present).

## Phasing

- **P1 — Ontology + write gate (§1).** `rel_types` table + seed, `rel_types`
  registry cache, `memory_fact_gate`, typed-edge population. Recall reads typed
  edges. *Default-off flag.* Self-contained and independently shippable.
- **P2 — Entity registry (§3).** Surrogate ids + aliases + conflict queue; edge
  writes resolve through it. Highest correctness leverage; depends on P1's write
  path.
- **P3 — Confidence classes + correction (§4, §5).** `confidence_class`,
  bitemporal columns, `correction_behavior` enforcement, retraction handler,
  promotion/expiry maintenance modes.
- **P4 — Self-extension (§2).** Promotion pipeline + `aimee expand`.
- **P5 — Cost + privacy (§6, §7).** Pattern-first extractor (coordinated with
  the cost ledger) + sensitivity gating.

Each phase lands behind a default-off config flag and graduates per the
flag-rollout-readiness program (6-criterion bar) — nothing flips on by default
in the same PR that introduces it.

## Trade-offs & risks

- **Two memory models coexist.** Free-text prose memory (great for code /
  episodes) and typed facts (great for identity / world) serve different recall
  shapes. Risk: ambiguity about which path owns a given assertion. Mitigation:
  the §1 gate only *accepts* a triple when it maps to a known/plausible
  rel_type; everything else stays prose. The two share the `entity_edges` table
  but are kept separable by the `edge_class` discriminator (§0) and unioned only
  at injection (§7) — not distinguished by an implicit heuristic.
- **Ontology quality gates correctness.** A wrong `head_kinds`/`tail_kinds`
  rejects legitimate facts. Mitigation: novel types stage as Class C (don't hard
  block), `aimee expand` is human-approvable, strict-match entity resolution
  avoids aggressive auto-merge.
- **Extraction precision.** Over-eager triple extraction pollutes the graph.
  Mitigation: Class B/C confidence + 3-confirmation promotion + 30-day expiry
  means unconfirmed extractions self-clean; regex patterns (§6) are tuned for
  precision over recall.
- **Migration weight.** One coherent migration up front (not the incremental
  churn this kind of schema tends to accrete); the seed ontology is small and
  W3C-flavoured (`instance_of` / `subclass_of` / `same_as`) for portability.

## Testing

- Unit: gate accept/reject matrix over `head_kinds`/`tail_kinds`, symmetric +
  inverse enforcement, correction_behavior (supersede/hard_delete/immutable),
  alias ON-CONFLICT preferred-preservation, near-match resolution (Theo≠Theodore
  but "DevBox"≈"devbox"), class promotion at 3 confirmations, C-class expiry,
  regex extractor precision, sensitivity withholding.
- Unit (R1): ontology self-validation (unknown `head_kinds` enum rejected at load;
  `is_symmetric`+`inverse_rel_type` inconsistency rejected; `correction_behavior`
  NOT-NULL backfill; `worksFor`≡`works_for` canonicalization); `edge_class`
  isolation (a typed recall walk never returns a `cooccurrence` row, and
  vice-versa); gate **fail-closed** when the ontology is unreachable;
  model-sourced triple cannot enter at Class A; `(kind, canonical_id)` uniqueness;
  single-hop alias resolution (no `A→B→A`); `entity_name_conflicts` lifecycle +
  escalation; `merge`/`unmerge` round-trip.
- Integration: end-to-end "state → correct → recall current → recall historical"
  over the `/v1/facts/*` surface — including same-session "X is Y → query Y"
  (synchronous binding) and both bitemporal axes (valid-time "used to" vs.
  transaction-time "believed last week"); user correction overriding an
  `immutable` type while a model correction is rejected; `aimee expand <domain>`
  seeds and a subsequent fact validates against the learned types.
- Build-integrity: new TUs wired into `CORE_SRCS` + the test object lists; the
  `/v1/facts/*` routes regenerated through the openapi + gen-cli-v1-routes path;
  `aimee git verify` (full `-Werror` build) green before push.

---

## Review revisions (R1)

Folded in from a multi-lens delegate review (reviewer / architect / security /
data-model passes). Each blocking finding and where it landed:

- **R1-A1 — shared storage vs. "not merged" claim** *(architect)*: added the
  `edge_class` discriminator (§0, data model, trade-offs) so typed and
  co-occurrence rows in the same `entity_edges` table are explicitly separable;
  union happens only at injection, keyed on `edge_class`.
- **R1-B3 — `memory_fact_gate` had no call site** *(reviewer)*: §1 now names the
  gate as the single commit point for all three triple emitters, the only setter
  of `edge_class='semantic'`, and **fail-closed** when the ontology is
  unreachable.
- **R1-B1 — `immutable` vs. "user authority is absolute"** *(reviewer)*: §4 now
  scopes `correction_behavior` to the inferred path; a user correction always
  supersedes (Class A), `immutable` only blocks model/inferred overwrites.
- **R1-B2 — strict-match didn't solve fragmentation** *(reviewer)*: §3 resolution
  is now two-tier and **synchronous** (immediate explicit binding + conservative
  audited near-match); the async queue is strictly for true ambiguity, and blocks
  neither write nor recall.
- **R1-B4 — "bitemporal" wasn't** *(reviewer)*: §4 names both axes explicitly —
  transaction time (`superseded_at`) and valid time (`valid_from`/`valid_to`) —
  with the query form for each.
- **R1-D1 — `rel_types` was self-trusted** *(data-model)*: §1 adds ontology
  load-time validation — closed `head_kinds`/`tail_kinds` enum, symmetric/inverse
  consistency, `correction_behavior` NOT-NULL default, `snake_case`
  canonicalization.
- **R1-D2 — registry constraints were assumed, not stated** *(data-model)*: §3
  adds UNIQUE `(kind, canonical_id)`, single-hop aliases (no circular chains),
  an `entity_name_conflicts` lifecycle, and first-class `merge`/`unmerge`.
- **R1-S1 — model trusted where it shouldn't be** *(security)*: §1 adds the trust
  boundary — model-sourced triples are capped at Class B/C (never Class A), and
  ontology-shape promotions remain human-approvable.

### R2 (second review round)

A second delegate pass on the R1 revision surfaced five more blocking issues,
all folded in:

- **R2-1 — `hard_delete` vs. §0 "never hard-delete"**: redefined as a
  suppress-and-archive *tombstone* (`suppressed` flag + `superseded_at`), so the
  origin artifact is always retained.
- **R2-2 — fail-closed was circular**: separated the two gate-failure modes —
  *novel rel_type with ontology reachable* → stage Class C; *ontology unreachable*
  → defer to a retry queue (C-staging needs the ontology, so it can't be the
  unreachable-path behaviour).
- **R2-3 — B→"permanent" was undefined**: promotion now elevates B from expiring
  to *durable B* (no TTL) but **never to Class A**; durable-B conflict/decay
  semantics stated explicitly.
- **R2-4 — "provisional id" had no schema home**: added a `status`
  (`active`/`provisional`/`merged`) + `merged_into` column to `entity_registry`.
- **R2-5 — uniqueness prose ≠ constraint**: `canonical_id` is now a
  globally-unique surrogate PK (matching the "never reused across kinds" claim),
  not a per-kind `UNIQUE (kind, canonical_id)`.

### R3 (third review round)

A third pass (security lens) found four more blocking issues, all folded in:

- **R3-1 — Class-C triple had a dangling `relation_id`**: novel types now insert a
  `provisional` `rel_types` row at stage time, so the edge's `relation_id` always
  resolves; promotion flips it `active`.
- **R3-2 — seed fallback contradicted "DB down → defer everything"**: the gate now
  validates seed-known types against the in-code `SEED_ONTOLOGY` during a DB
  outage; only *novel* writes during an outage defer.
- **R3-3 — alias tombstone was unbuildable**: added a `suppressed` flag to
  `entity_aliases`, symmetric with `entity_edges`, so `hard_delete` suppresses an
  alias while retaining the audit row.
- **R3-4 — `sensitivity` could fail open**: closed enum (`normal`/`pii`/`secret`),
  `NOT NULL`, default `pii`; unclassified types are withheld from injection by
  default (§7 fails closed).

### Convergence

A fourth round on R3 came back **clean across three lenses and two models** —
reviewer (minimax), architect (mistral), and security (mistral) each returned
*NO MAJOR ISSUES*. The review loop is considered converged: findings progressed
structural (R1) → state-semantics (R2) → schema-completeness/fail-open (R3) →
clean, with no new blocking issues on the final pass.

The one item carried as deferred — `confidence_class` A/B/durable-B/C transition
semantics and its interaction with the existing scalar `confidence` /
`provenance_category` / lineage `source_kind` fields (§0, §5) — was then given a
dedicated review pass and also returned *NO MAJOR ISSUES*. No open review items
remain; the proposal is ready for human sign-off.
