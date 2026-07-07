# Proposal: LLM-sidecar productionization — graduate curator extraction and idle reflection from stub to production

- **State:** PENDING — design only, no code in this PR. Finalises two
  scaffolded-but-stubbed intelligence steps that ride the shared
  `kb_curator_sidecar` mechanism. Adds no
  new subsystem: it wires the existing curator extract/synthesize/judge stages and
  the existing idle-reflection scheduler onto a production sidecar, behind a
  calibrated gate. Continuation of **Deep Curator: Doc and Code Extraction**
  (Accepted, Phase 0 shipped — `scripts/embed-minilm.py` MiniLM-L6-v2 embedding
  sidecar) and the **Cross-Source Learning** substrate (Done). Does **not**
  re-propose the curator stage machine, the reflection scheduler, the promotion
  pipeline, calibration, or the sidecar-invocation shim — all shipped; it makes the
  LLM step they were built around real.
- **Author:** JBailes
- **Date:** 2026-07-07
- **Charter roles:** Extract (structured entity/fact/decision/relationship
  extraction from docs and code), Synthesize (reflection candidate generation +
  higher-order knowledge), Judge (quality gate before durable), Reflect
  (idle-time consolidation), Calibrate (per-surface promotion thresholds already
  fitted — this feeds them real candidates), Gate-Promote (staged rollout of the
  sidecar's output), Evaluate-Optimize (shadow → canary → default via the bandit
  substrate). Cites the Architecture Charter; this proposal lives entirely inside
  the charter's Extract/Synthesize/Judge/Reflect spine.

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

Both stubs share one dependency: a production LLM sidecar with a stable contract.
This proposal defines that contract once, graduates both consumers onto it behind
a calibrated shadow→canary→default gate, and closes the two operational-validation
items those stages left open. When it lands, the §1/§3 claims become true in the
build, not just in the architecture.

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

## Explicitly out of scope / does not re-propose

- The curator stage machine, queue/drain, versioning, judge, promotion, and
  calibration/bandit rails — all shipped; reused verbatim.
- Non-file source ingestion (Jira/Slack/email/Drive/…) — that on-ramp is the
  sibling proposal `org-data-connectors-and-source-ingestion.md`; this proposal
  makes extraction real, that one makes the corpus wide. They compose but ship
  independently.
- Any change to the typed-fact ontology or write gate — relationship candidates
  ride the existing `rel_types` gate unchanged.
