# Curator pipeline

The **curator pipeline** turns ingested content (docs, code) into curated,
retrievable knowledge — searchable claims, resolved entities, contradiction links,
a projection graph — in the background, after the fast 0.6B embedder has made the
raw corpus searchable. It runs entirely inside `aimee-kb` on a drain thread; there
is no separate scheduler.

Since the modular-pipeline refactor it is a **data-driven stage registry**, not a
hardcoded chain. Each stage is one entry in an ordered list; the drain flows work
through the enabled stages. This is what makes the pipeline composable (different
applications enable different stages) and lane-separated (CPU work runs concurrently
with GPU work).

## The stage registry

Defined in `src/kb/kb_curator_pipeline.h` and populated in `src/kb/kb_curator_drain.c`
(`CURATOR_STAGES[]`). Each stage is:

```c
typedef struct {
   const char *name;   /* stable id / config + GUI key, e.g. "index_claims" */
   const char *label;  /* human status label */
   int (*enabled)(const config_t *cfg);            /* NULL => always on */
   int (*run)(const kb_curator_extract_opts_t *);  /* one unit: 1=did work, 0=idle, <0=error */
   int budget;                                     /* max units per pass (<=0 => 1) */
   kb_curator_lane_t lane;                         /* LLM (GPU) or INDEX (CPU) */
} kb_curator_stage_desc_t;
```

`kb_curator_pipeline_run_pass(stages, n, lane_filter, cfg, opts, set_status)` walks
the registry: for each enabled stage whose lane matches the filter, it runs the
stage up to `budget` times (or until the stage's queue empties), in order, stopping
at the first stage that returns an error. It returns `1` if any stage did work
(caller loops), `0` if all idle (caller sleeps), `-1` on a stage error (caller backs
off). It is dependency-free and unit-tested with mock stages
(`test_curator_pipeline_sched`).

## Resource lanes

Stages bottleneck on different hardware, so they run on **two worker threads** with
independent cadences (`kb_curator_drain_init` spawns both):

| Lane | Hardware | Cadence | Stages |
|------|----------|---------|--------|
| **`KB_CURATOR_LANE_LLM`** | GPU / LLM | one unit per pass (each is a multi-second model call) | extract_docs, extract_code, resolve_entities, synthesize, promote_entity |
| **`KB_CURATOR_LANE_INDEX`** | CPU (0.6B embedder / SQL) | drains its queue each pass, loops hot | index_narrative, index_claims, detect_contradictions, index_code_unit, link_artifacts, embed_code, ingest_docs, embed_evidence |

The **main drain thread** runs the LLM lane (`run_pass(..., KB_CURATOR_LANE_LLM, ...)`)
plus a few global bookkeeping sweeps (cross-repo cold-start, decision-revisit,
typed-facts, projection-graph). The **index-lane thread**
(`kb_curator_index_lane_main`) runs the INDEX lane and backs off `INDEX_LANE_POLL_SECS`
when idle. Because they are separate threads, CPU indexing proceeds **while the GPU
is mid-extraction** — a document's claims get embedded and contradiction-checked
while the next document is still being extracted, using CPU that would otherwise
idle during each LLM call.

The lanes touch mostly disjoint data; where they meet (the `artifacts` table) it is
producer/consumer — the LLM lane INSERTs proposed artifacts, the INDEX lane
SELECTs + UPDATEs them — which is safe under Postgres MVCC and the existing
`FOR UPDATE SKIP LOCKED` claim.

## The stages

In registry order (the pass order). Each has a `kb_curator_<name>_enabled` config
flag (default on) — see below.

**LLM lane (GPU):**
- **extract_docs** — LLM-extract a document chunk into claims / entities / doc_summary artifacts (`{status:"ok", artifacts:[{kind,payload}]}`, enforced by a json-schema so the reasoning model returns fence-free JSON).
- **extract_code** — extract a code symbol into a code_unit artifact.
- **resolve_entities** — resolve entity-mention artifacts against the canonical entity graph.
- **synthesize** — synthesize a topic summary from committed artifacts.
- **promote_entity** — promote a recurrent entity to the canonical registry.

**INDEX lane (CPU):**
- **index_narrative** — embed doc_summary / narrative text.
- **index_claims** — embed each `claim` artifact's `subject/attribute/value` into `curator_claim_vectors`.
- **detect_contradictions** — self-join `curator_claim_vectors`: same `subject`+`attribute`, different `value` → write a `contradicts` artifact_link.
- **index_code_unit** — embed code_unit artifacts.
- **link_artifacts** — link related artifacts (implements / relates-to).
- **embed_code** — embed changed files into `code_embeddings` (the code-vector layer).
- **ingest_docs** — chunk + embed prose/doc files into `kb_documents` (the in-ingest replacement for `kb build`); self-heals missing embeddings and reconciles the dim-change re-embed marker.
- **embed_evidence** — fill `evidence_vectors` for the neighbourhood builder.

## Enabling / reordering stages

Every stage's `enabled` predicate reads a config flag
(`kb.curator.<stage>.enabled`, all default-on via `config_kb_curator_defaults`).
Toggling a flag turns a stage into a no-op without touching code. The registry order
is the pass order. This is the surface a **GUI pipeline editor** drives: an
application composes its own pipeline by toggling stages — e.g. a doc-QA app runs
extract_docs → index_claims → detect_contradictions; a code-intel app runs
extract_code → index_code_unit; a story-world app runs an extract_story variant.

## End-to-end: how a contradiction is found

1. `ingest_docs` chunks + embeds a doc into `kb_documents`.
2. The ingest hook (or the drain backfill `kb_curator_queue_docs_all_projects`) enqueues an `extract_doc` job.
3. `extract_docs` (LLM lane) turns the chunk into `claim` artifacts `{subject,attribute,value}` (state `proposed`).
4. `index_claims` (INDEX lane) embeds each claim into `curator_claim_vectors`.
5. `detect_contradictions` (INDEX lane) self-joins the vectors; two claims with the same subject+attribute but different value get a `contradicts` link.

Because the INDEX lane runs concurrently, steps 4–5 proceed for one document while
step 3 runs for the next.

## Adding a stage

1. Write a worker `int kb_curator_<name>_one(const kb_curator_extract_opts_t *opts)` returning `1`/`0`/`-1` (or an adapter wrapping an existing per-project drain — see `stage_embed_code` etc. in `kb_curator_drain.c`).
2. Add an `en_<name>` predicate reading its config flag.
3. Add one row to `CURATOR_STAGES[]` with the right lane + budget (LLM stages: budget 1; cheap INDEX stages: a larger budget so they drain their queue each pass).

No drain-loop code changes are needed — the registry drives it.

## Source map

- `src/kb/kb_curator_pipeline.{h,c}` — stage descriptor + `run_pass`.
- `src/kb/kb_curator_drain.c` — the registry (`CURATOR_STAGES[]`), the two lane workers, the stage adapters.
- `src/tests/test_curator_pipeline_sched.c` — `run_pass` scheduling (lane filter / budget / error-stop).
- `src/tests/test_curator_pipeline.c` — end-to-end curator chain over the sqlite shim.
