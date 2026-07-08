# Proposal: User-configurable curator pipeline — reorder, constraints, presets, user-defined stages

- **State:** PENDING — design + phased build. Authored autonomously overnight
  (2026-07-08) at the product owner's direction ("users order stages any valid
  way; enforce constraints and disallow impossible configs; presets starting at 3,
  ultimately user-defined; ultimately users add their own stages"). The design
  decisions here would normally be validated by the delegate roundtable, but the
  roundtable was unavailable this session (narrow default panel; the `.254`
  `ensemble.reference_models` config edit failed — codex delegate exhausted budget
  with "glob failed"). Flagged for review.
- **Author:** JBailes (assisted)
- **Date:** 2026-07-08
- **Builds on:** the modular curator pipeline (Phases 1–5, shipped): the
  data-driven stage registry `CURATOR_STAGES` (`src/kb/kb_curator_drain.c`), the
  two resource lanes (LLM=GPU, INDEX=CPU) with per-lane worker threads via
  `kb_curator_pipeline_run_pass`, the `curator.stages` registry endpoint (Option B,
  single source of truth), and built-in presets (#1173).

## 1. The order model — what "order" actually means here

The pipeline is **queue-decoupled and two-laned**: each stage drains its own
queue each pass; a producer (e.g. `index_claims`) enqueues work a consumer (e.g.
`detect_contradictions`) picks up. The two lanes run on **separate threads**
concurrently. Consequences:

- **Cross-lane order is not a thing** — the LLM and INDEX lanes run in parallel;
  you cannot sequence an INDEX stage "before" an LLM stage. Only **intra-lane**
  order is meaningful.
- **Intra-lane order affects latency, not correctness** — because queues persist
  across passes, running a consumer before its producer in one pass just defers
  its work to the next pass; it never corrupts output. Correctness is guaranteed
  by the queues regardless of order.

So "reorder" is an **intra-lane, latency-affecting** control, and the dependency
constraints exist to keep orders **sensible** (producer before consumer within a
lane), not to prevent data corruption. This is why we *enforce* a DAG (disallow
orders that invert a producer→consumer edge) rather than merely warn: it matches
the operator's mental model and prevents pointless/confusing orders, even though
the queue layer would tolerate them.

## 2. Dependency DAG (the constraints)

Producer→consumer prerequisites (a stage may not be ordered before a prerequisite
**in the same lane**; cross-lane prerequisites are advisory and shown in the GUI
but not order-enforced since lanes are concurrent):

| stage | requires |
|---|---|
| resolve_entities | extract_docs |
| index_narrative | extract_docs |
| index_claims | extract_docs |
| detect_contradictions | index_claims *(intra-lane — enforced)* |
| index_code_unit | extract_code |
| link_artifacts | extract_docs, extract_code |
| synthesize | index_claims |
| promote_entity | resolve_entities |
| embed_evidence | index_claims *(intra-lane — enforced)* |
| cross_repo_graph | projection_graph *(intra-lane — enforced)* |

`extract_docs`, `extract_code`, `embed_code`, `ingest_docs`, `projection_graph`
have no prerequisites. The enforced intra-lane edges (INDEX lane) are the ones
that actually gate a valid order.

## 3. Reorder — representation, validation, runtime

- **Config:** `kb.curator.stage_order` — a comma-separated list of stage names.
  Empty ⇒ registry order (the current default). Unknown names ignored; omitted
  stages keep registry order after the listed ones.
- **Validation:** `kb_curator_order_valid(order, &reason)` — every stage must
  appear after its intra-lane prerequisites. **Disallow** invalid orders: the GUI
  refuses the move client-side (using the `requires` from the endpoint), the
  `config.set` path rejects an invalid `stage_order`, and the runner **fails safe**
  (invalid config ⇒ registry order + one WARN) as defense-in-depth.
- **Runtime:** each lane worker builds a validated, reordered view of
  `CURATOR_STAGES` per poll and passes it to `kb_curator_pipeline_run_pass`
  (already parameterised by a stages array). Cheap (≤15 entries).
- **Endpoint:** `curator.stages` gains `requires: [name,...]` per stage so the GUI
  can enforce/visualise constraints.

## 4. Presets

- **v1 (shipped, #1173):** built-in profiles (full / docs-only / code-only),
  backend-defined, served on `curator.stages`; applying sets the stage flags.
- **v2 (this proposal):** user-defined/saved presets in `kb.curator.presets`
  (`{name: [config_key,...]}`), merged with the built-ins in the endpoint; a
  `curator.save_preset` / `curator.delete_preset` op writes the config section
  (config.set only does flat keys). GUI: name + save the current toggles; apply/
  delete saved profiles.

## 5. User-defined stages (the hard part)

A stage today is a C struct with a **function pointer** (`run`). Arbitrary
user-supplied code is a non-starter (correctness + security). Phased, safe path:

- **v1 — composed stages:** a user stage is `{name, base_op, lane, budget}` in
  `kb.curator.custom_stages`, where `base_op` **references an existing, vetted
  registry op** (e.g. an operator defines a second `index_claims`-style pass over a
  different source, or renames/relanes an existing op). The registry the workers
  iterate becomes `built-in ⊕ custom` (custom validated against the same DAG rules
  + lane legality). No new executable code — only recomposition of shipped ops.
- **v2 — declarative stages:** a small, sandboxed spec (source selector →
  transform op → sink) built from a fixed vocabulary of vetted primitives. Still
  no arbitrary code.
- **v3 — plugin stages:** the existing plugin/hook mechanism (`plugin_loader`,
  `plugin_c_hook`) exposes a registration hook so a signed plugin contributes a
  stage. This is where genuinely new stage *logic* lives, gated by the plugin
  trust model — not user config.

Full arbitrary user code is explicitly out of scope.

## 6. Phase plan

- **Phase A** — DAG + `requires` on the endpoint; `kb.curator.stage_order` config;
  validation; runtime reorder (fail-safe). *(core buildable now)*
- **Phase B** — GUI: drag/▲▼ reorder with client-side constraint enforcement;
  persist `stage_order`.
- **Phase C** — user-defined presets (save/apply/delete). *(builds on #1173)*
- **Phase D** — composed user-defined stages (`custom_stages`), DAG- and
  lane-validated.
- **Phase E** — plugin-contributed stages (future; trust-gated).

## 7. Risks

- **Runner change (Phase A)** touches the core drain loop. Mitigated by: reorder is
  latency-only (queues protect correctness), fail-safe fallback to registry order,
  and no change to the stage set or their run functions.
- **`config.set` rejecting invalid orders** needs a per-key validation hook — small
  but new surface; keep the runner fail-safe regardless.
- **Composed stages** must validate `base_op` against the vetted op set and reject
  unknown ops (no code injection); lane legality enforced.

## 8. Acceptance

- Invalid `stage_order` is rejected at the API and the GUI, and the runner logs one
  WARN and uses registry order if a bad order reaches config.
- A valid reorder changes the per-pass stage sequence within a lane (observable in
  the drain debug log) without changing curated output.
- Saved user presets round-trip through config and apply correctly.
- A composed custom stage runs its `base_op` on its lane and is DAG-validated.
