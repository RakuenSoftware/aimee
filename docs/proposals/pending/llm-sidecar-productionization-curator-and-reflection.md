# Proposal: LLM-sidecar productionization — graduate curator extraction and idle reflection from stub to production

- **State:** PENDING — design only, no code in this PR. Finalises three
  scaffolded-but-stubbed intelligence steps that ride the shared
  `kb_curator_sidecar` / offline-extractor mechanism. Adds no new **durability**
  subsystem: it wires the existing curator extract/synthesize/judge stages, the
  existing idle-reflection scheduler, and the existing `memory_facts` drain onto a
  production sidecar behind a calibrated gate, and reuses the shipped `rel_types`
  write gate and ontology-evolution machinery for reconciliation. The one genuinely
  new surface is the KB-owned Typed Facts console panel (§8). Continuation of
  **Deep Curator: Doc and Code Extraction**
  (Accepted, Phase 0 shipped — `scripts/embed-minilm.py` MiniLM-L6-v2 embedding
  sidecar) and the **Cross-Source Learning** substrate (Done). Does **not**
  re-propose the curator stage machine, the reflection scheduler, the promotion
  pipeline, calibration, or the sidecar-invocation shim — all shipped; it makes the
  LLM step they were built around real. Extended (2026-07-07) to fold in the
  **memory typed-fact path** (`kb_memory_facts.c`), which rides the same offline
  extractor: move its extraction fully offline, default it on, add **autonomous
  ontology reconciliation** so extracted facts actually become durable and
  recallable (not just logged), and surface the whole subsystem — observe,
  fine-tune, and alter behaviour — in **aimee-kb's web console**, backed by
  **KB-owned config**. aimee-server owns none of it.
- **Author:** JBailes
- **Date:** 2026-07-07
- **Charter roles:** Extract (structured entity/fact/decision/relationship
  extraction from docs, code, and remembered notes), Reconcile (map free-form
  extracted relations onto the canonical `rel_types` ontology, and auto-promote
  the genuinely novel tail so facts become durable without a human), Synthesize
  (reflection candidate generation + higher-order knowledge), Judge (quality gate
  before durable), Reflect (idle-time consolidation), Calibrate (per-surface
  promotion thresholds already fitted — this feeds them real candidates and drives
  autonomous ontology promotion), Gate-Promote (staged rollout of the sidecar's
  output), Evaluate-Optimize (shadow → canary → default via the bandit substrate).
  Cites the Architecture Charter; this proposal lives entirely inside the charter's
  Extract/Reconcile/Synthesize/Judge/Reflect spine.

## Thesis

The knowledge base's "it learns, not just stores" story (KNOWLEDGE.md §1, §3) rests
on two steps that are **scaffolded but stubbed today**:

1. **Curator extraction.** Every curator stage exists as shipped code —
   `kb_curator_extract.c`, `kb_curator_extract_code.c`, `kb_curator_judge.c`,
   `kb_curator_synthesize.c`, `kb_curator_resolve_entities.c`,
   `kb_curator_link_artifacts.c`, `kb_curator_contradictions.c`,
   `kb_curator_grounding.c`, `kb_curator_index_narrative.c`,
   `kb_curator_index_claims.c`, `kb_curator_promote.c` — draining
   `corpus_processing_jobs` through the staged pipeline (`aimee kb pipeline`). But
   the LLM extraction they exist to run reaches the model through
   `kb_curator_sidecar_run(cmd, json_input, …)` (`kb_curator_sidecar.c`), and the
   only sidecar shipped so far is the **Phase 0 embedding** sidecar. Entity / fact
   / decision extraction and the doc↔code `implements` bridge do not yet run
   against a production extractor.

2. **Idle reflection.** `kb_reflection.c` ships a full idle-triggered scheduler
   (fires when `now − last_session_rpc_ts > review_idle_trigger_minutes`, over
   unreflected `session_summary` artifacts older than
   `review_session_cooldown_hours`, stamping `reflected_at`, deduplicating). Its
   own header states the gap verbatim: *"LLM candidate generation is stubbed
   pending charter sidecar integration; the scheduler infrastructure and
   deduplication run fully."* So aimee reflects **structurally** today; it does not
   yet **synthesize**.

3. **Memory typed-fact extraction — runs, but nothing lands.** The `memory_facts`
   drain (`kb_memory_facts.c`, on `kb_curator_drain.c`) already runs the LLM
   extractor (`kb_curator_llm_run`, `MF_SYSTEM_PROMPT`) offline over stored
   memories. But two things keep it from being real: (a) a redundant *synchronous*
   pattern pass still fires on the store/turn hot path (`db2_typed_fact_ingress` →
   `db2_fact_ingest_text` in `kb_service_backend_memory.c` / `fact_ingest.c`); and
   (b) the extractor emits **free-form** relations that miss the seed ontology, so
   the write gate stages them provisional and **no durable, recallable fact is
   written**. Validated on the .254 stack (2026-07-07): a note storing
   `is_cto_of` / `drives` / `has_office_in` logged *"memory 7 → 3 typed facts"*
   while `typed_facts` stayed empty, `rel_types` held all three as
   `status=provisional`, and recall returned nothing. Extraction happens; usable
   facts do not. And none of it is observable or tunable by an operator.

These steps share one dependency: a production LLM extractor, run offline, whose
output is **reconciled** to a canonical ontology before the promotion gate. This
proposal defines the sidecar contract once, graduates all three consumers onto it
behind a calibrated shadow→canary→default gate, makes the memory typed-fact path
offline-and-default-on with autonomous reconciliation, surfaces the whole
subsystem as **KB-owned** config + an aimee-kb console panel, and closes the two
operational-validation items those stages left open. When it lands, the §1/§3
claims become true in the build, not just in the architecture.

## Goal

1. **One production sidecar contract** — a versioned, JSON-in/JSON-out request/
   response schema (`sidecar_contract_version`) shared by curator extraction and
   reflection synthesis, invoked through the existing `kb_curator_sidecar_run`
   shim, with a CPU-first reference implementation that installs today.
2. **Curator extraction runs it** — the extract/judge/synthesize stages produce
   real entities, facts, decisions, relationships, and the doc↔code `implements`
   bridge, gated by the shipped judge stage.
3. **Reflection synthesizes** — the reflection scheduler's stubbed candidate
   generation calls the same sidecar and emits synthesis candidates into the
   shipped promotion pipeline.
4. **Gated rollout, never a silent flip** — every sidecar output lands in shadow
   first, is scored against a held-out fixture set, and is promoted to default only
   through the shipped Bayesian-calibration + bandit machinery.
5. **Close the open operational items** — the two Operational Validation Cycle
   proposals (Working-Profile, Dogfood Autolabel) get their first real
   candidate stream from this, closing their "unshipped operational artifacts"
   remainder on a real calendar.
6. **Memory typed facts, offline and default-on** — extraction (pattern + LLM)
   runs entirely on the `memory_facts` drain; the store/turn hot path keeps only
   cheap Postgres retraction + recall. `typed_facts_enabled` defaults **on** on
   every backend (including CPU-only E4B), because nothing is synchronous LLM.
7. **Autonomous ontology reconciliation** — extracted relations are reconciled to
   the canonical ontology (constrain-the-extractor + auto-promote the novel tail)
   **by default**, so facts become durable and recallable with no operator action.
8. **KB-owned, GUI-tunable** — every knob (enable, reconciliation mode, thresholds,
   ontology) lives in **aimee-kb** config and its web console — observe, fine-tune,
   and alter behaviour there. **aimee-server knows nothing about typed facts**; its
   per-turn injection just asks the KB and renders whatever the KB returns.

## §0 What already exists (so we don't rebuild it)

- **Sidecar shim.** `kb_curator_sidecar_run(cmd, json_input, out_cap, errbuf,
  errlen)` (`src/kb/kb_curator_sidecar.c`) — spawns a configured command, pipes
  JSON in, reads JSON out, fails closed with an error string. Backend-agnostic.
- **Curator stage machine.** All stages listed in the Thesis, plus
  `kb_curator_queue.c` / `kb_curator_drain.c` (queueing + deterministic drain),
  `kb_curator_version.c` (prompt/model version keying), `kb_curator_llm.c` (LLM
  call plumbing), `kb_curator_notify.c`. Driven by `corpus_processing_jobs`
  (`aimee kb pipeline`) — the resumable per-document stage machine (Done).
- **Curator config.** `src/config_kb_curator.c` + `src/curator_profile.c` — the
  CPU-first curator profile and sidecar command configuration.
- **Reflection scheduler.** `kb_reflection.c` — idle trigger, cooldown, dedup,
  `reflected_at` stamping; surfaced as `reflection-scheduler` in
  `kb_service_workers.c`. Everything but LLM candidate generation runs.
- **Promotion + calibration.** The charter promotion pipeline, **Bayesian
  Calibration of Promotion Thresholds** (Done — per-`(surface, kind, scope)`
  Beta-binomial posteriors, conformal abstention floor, working-profile gate
  consumption), and **Contextual Bandits and Counterfactual Replay** (Done — five
  decision points wired, exploration-budget gate, replay attribution). These are
  the rollout rails; today they have no real curator/reflection candidate stream to
  rank.
- **Embedding sidecar (Phase 0).** `scripts/embed-minilm.py` (MiniLM-L6-v2) — the
  pattern this proposal generalizes from embeddings to extraction/synthesis.
- **Judge stage.** `kb_curator_judge.c` — the quality gate every candidate already
  passes before becoming durable.

## §1 The sidecar contract (`sidecar_contract_version`)

One versioned JSON contract, shared by both consumers, invoked through the existing
shim. A request carries `{contract_version, task, profile, inputs[], budget}`;
`task ∈ {extract_doc, extract_code, synthesize_reflection}`. A response carries
`{contract_version, candidates[], usage, abstained[]}` where each candidate is a
typed artifact (entity / fact / decision / relationship / synthesis) with
provenance spans and a self-reported confidence — **never** a durable write; the
judge stage (§2) and promotion gate (§4) decide durability.

- **Version-keyed.** `contract_version` composes with the shipped prompt/model
  version keying (`kb_curator_version.c`) so a bump replays cleanly and calibration
  refits (already keyed on prompt/model version).
- **Fails closed.** A malformed or empty sidecar response is a *defer* (retry),
  never a false success — matching the typed-fact write-gate discipline and the
  existing `_extract.c` `pending`/`failed` attempt accounting.
- **Backend-pluggable.** The reference implementation is CPU-first (small local
  model, consistent with the curator profile); an operator can point
  `kb.curator.sidecar_cmd` / a new `kb.reflection.sidecar_cmd` at any command
  honoring the contract. No cloud dependency required to install.

## §2 Curator extraction on the real sidecar

- Wire `kb_curator_extract.c` / `_extract_code.c` to emit `task=extract_doc` /
  `extract_code` requests and route responses into the existing
  resolve-entities → link-artifacts → contradictions → judge → promote path. No new
  stages.
- Land the **doc↔code `implements` bridge** (Deep Curator's headline edge) as a
  relationship candidate kind produced by `extract_code` and validated by the
  typed-fact ontology gate — reusing the shipped `rel_types` machinery, not a new
  one.
- Contradictions and grounding stages already exist; they now receive real
  candidates instead of Phase-0 embeddings.

## §3 Reflection synthesis on the real sidecar

- Replace the stubbed candidate generator in `kb_reflection.c` with a
  `task=synthesize_reflection` call over the unreflected-`session_summary` batch
  the scheduler already assembles.
- Emit synthesis candidates into the same promotion pipeline curator output uses —
  one durability path, one judge, one calibration surface.
- Keep every shipped guardrail: idle trigger, cooldown, dedup, `reflected_at`
  stamping stay exactly as-is; only the "what runs at the fire point" changes.

## §4 Gated rollout — shadow → canary → default

No silent flip. Sidecar output graduates through the shipped rails:

1. **Shadow.** Candidates are generated and scored but not promoted; logged as
   evidence. Mirrors the semantic-guardrails `dry_run` shadow mode already in the
   tree.
2. **Fixture gate.** A held-out extraction/synthesis fixture set (built like the
   sketch and bandit fixtures already in the repo) gates promotion: precision on
   entities/facts and a synthesis-usefulness proxy must clear a threshold before
   canary.
3. **Canary → default.** Promotion thresholds come from the fitted
   `calibration_profile` artifacts (Bayesian calibration, Done), and the
   default-flip decision is a bandit arm (`sidecar_extraction_mode`,
   `reflection_synthesis_mode`) flipped through the shipped
   `POST /v1/intelligence/bandit/replay-record` path — attribution recorded as
   `benchmark_trace` artifacts, exactly as the five existing decision points do.

## §5 Closes the open operational items

The two **Operational Validation Cycle** proposals (Working-Profile, Dogfood
Autolabel) exist to close acceptance items from shipped plumbing on a real
calendar; both were blocked on having a live candidate stream. This proposal
produces that stream — the first end-to-end pass of the promotion + retroactive-
review loop is a curator/reflection candidate cohort moving shadow→default. Their
close-by deadlines become achievable, not hypothetical.

## §6 Memory typed facts — offline, default-on, KB-owned

- **Extraction fully offline.** The `memory_facts` drain already runs the LLM
  extractor off the hot path; fold the remaining synchronous **pattern** pass
  (`memory_extract_patterns` via `db2_fact_ingest_text`) into the drain too, so the
  store/turn path (`kb_service_backend_memory.c`, `fact_ingest.c`) does **zero**
  synchronous extraction — it only enqueues the `memory_facts` job. Retraction
  stays synchronous (a cheap Postgres write, corrections take effect immediately);
  recall stays synchronous (`db2_fact_recall_in_query`, a Postgres read). Penalty:
  a fact stated in the current turn is recallable one drain-cycle later, not
  same-turn — an accepted trade for a cross-turn memory.
- **Default on, every backend.** `typed_facts_enabled` defaults **on** and is no
  longer tied to the `accel` signal (`config_apply_inference_backend_defaults`):
  since extraction is offline and recall is a DB read, there is no per-turn LLM
  cost on CPU-only E4B/E2B either. HyDE query rewrite — genuine per-turn LLM work —
  stays accel-gated and is untouched.
- **KB-owned.** The flag and every reconciliation/tuning knob live in **aimee-kb**
  config (a `kb.typed_facts.*` section) and the KB console (§8). aimee-server has
  **no** typed-fact gate: `ingress_preinject` calls the KB facts endpoint
  (`kb_client_memory_facts`), which returns facts or empty from the KB's own
  config. (Validation 2026-07-07: enabling it on aimee-server did nothing because
  the KB never saw the setting — this removes that split-brain by design.)

## §7 Autonomous ontology reconciliation

The write gate (`db2_fact_commit`, `rel_types_store.c`) only makes a fact durable
and recallable when its relation is **active** in the ontology; a free-form
relation is staged as a `provisional` `rel_type` plus a low-confidence Class-C
edge in `entity_edges` and never surfaces on recall. The extractor
(`MF_SYSTEM_PROMPT`) emits free-form snake_case, so on a bare system **every fact
is provisional** (the .254 evidence above). A human cannot be the one mapping
`is_cto_of → has_role`; aimee reconciles autonomously, **on by default**:

1. **Constrain the extractor.** Pass the seed ontology (`SEED_ONTOLOGY[]`,
   `rel_types.c`: `works_for`, `has_role`, `lives_in`, …) into the extraction
   prompt and instruct the model to map to the **nearest canonical** relation,
   using an `OTHER` fallback only when nothing fits. Most facts then commit
   `ACCEPT` → active → recallable immediately, with no promotion wait.
2. **Auto-promote the novel tail.** Genuinely out-of-ontology relations that recur
   across sources are promoted `provisional → active` by the existing
   ontology-evolution machinery, run **by default** on a calibrated threshold
   (reuse the shipped Bayesian-calibration substrate, §0). The ontology grows
   itself; the `OTHER`-bucket is drained over time rather than lost.

The default path needs no operator input. Every parameter — the ontology set,
the reconciliation mode, the promotion threshold, the confidence floor — is an
override exposed in §8, never a prerequisite.

## §8 aimee-kb console — observe, fine-tune, alter behaviour (KB-owned)

The KB console today is a single `/v1/console/overview` (`kb_http_console.c`). Add
a **Typed Facts** panel, KB-served and backed entirely by KB config —
aimee-server renders and knows nothing. Scoped to a shippable **core** now, with a
richer surface deferred:

**Core (this PR):**
- **Observe** (`GET /v1/console/typed_facts`) — the KB-owned config (enabled,
  auto_promote, promote_threshold) plus the provisional-relation promotion review
  queue (each candidate's observation count, ready-flag, status).
- **Alter behaviour + fine-tune** (`POST /v1/console/typed_facts/config`) —
  `enabled`, `auto_promote`, `promote_threshold`, round-tripped through
  `kb.typed_facts.*` and equally settable headless (same keys, no GUI required).
- **Act** (`POST /v1/console/typed_facts/relation`) — promote / map / reject a
  provisional relation by hand (the shipped ontology-evolution verbs), with the
  relation/target validated to the canonical `rel_type` form at the route boundary.

**Deferred (follow-on PR):** durable-vs-provisional fact counts, the live ontology
listing + per-candidate supporting evidence, and drain throughput/lag on the
observe surface; and the additional `kb.typed_facts.*` knobs (reconciliation mode,
confidence floor, ontology extensions/aliases, ontology auto-growth). These extend
the same routes and config section; nothing about aimee-server changes.

aimee-server is not in the loop for any of it.

## Acceptance criteria

1. **Contract.** `sidecar_contract_version` schema documented; both consumers emit
   it through `kb_curator_sidecar_run`; malformed response ⇒ defer (proven by a
   fixture that feeds garbage and asserts retry, not durable write).
2. **Curator.** `extract_doc` / `extract_code` produce entities, facts, decisions,
   relationships, and at least one `implements` doc↔code edge on a fixture corpus,
   all passing the shipped judge stage; contradictions stage exercised.
3. **Reflection.** An idle fire over a fixture `session_summary` batch produces
   synthesis candidates into the promotion pipeline; `reflected_at` stamped;
   dedup holds across two consecutive fires.
4. **Gate.** Shadow mode emits scored-but-unpromoted candidates; the fixture gate
   blocks a deliberately-poor sidecar from canary; the default flip is a recorded
   bandit decision, not a config edit.
5. **CPU-first install.** A reference sidecar honoring the contract runs with no
   cloud dependency and installs via the existing curator-profile path.
6. **Validation-pending, stated as such.** Real-corpus precision/usefulness numbers
   are a dogfood deliverable (§5); this proposal ships the plumbing + fixtures and
   reports corpus-scale quality as *validation-pending*, not done.
7. **Offline + default-on (KB).** With `typed_facts_enabled` at its default, a
   fresh KB on any backend (including CPU-only E4B) extracts with **zero**
   synchronous LLM on the store/turn path (proven by timing a store), and
   `aimee config` on aimee-server exposes no typed-fact knob at all.
8. **Reconciliation yields recallable facts.** On a fixture note, an extracted
   fact commits with a canonical (or auto-promoted) relation as an **active** edge
   and is returned by recall — proven end-to-end on the .254 stack, not merely
   "N facts logged." (This is the exact failure §7 fixes.)
9. **KB console (core).** The Typed Facts panel observes the KB-owned config +
   promotion review queue, and its enable / auto_promote / promote_threshold / act
   (approve, map, reject) controls round-trip through `kb.typed_facts.*`;
   aimee-server has no equivalent surface. (Richer observe metrics + the extra
   knobs are the deferred follow-on in §8.)

## Explicitly out of scope / does not re-propose

- The curator stage machine, queue/drain, versioning, judge, promotion, and
  calibration/bandit rails — all shipped; reused verbatim.
- Non-file source ingestion (Jira/Slack/email/Drive/…) — that on-ramp is the
  sibling proposal `org-data-connectors-and-source-ingestion.md`; this proposal
  makes extraction real, that one makes the corpus wide. They compose but ship
  independently.
- **A new ontology store or a rewrite of the write gate.** Reconciliation (§7)
  reuses the existing `rel_types` gate (`db2_fact_commit`), the seed ontology
  (`SEED_ONTOLOGY[]`), and the shipped ontology-evolution / calibration machinery
  verbatim. What is new is *feeding* the gate canonical relations (extractor
  constraint) and *running* provisional→active promotion on by default — the gate
  and the durability contract themselves are unchanged. Free-form relations are no
  longer silently stranded as provisional; that is the whole point.
