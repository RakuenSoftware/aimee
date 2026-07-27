# Gemma unified-role A:B fixtures

`build_254_fixtures.py` creates one frozen, paired evaluation suite from the live
`.254` KB without persisting raw production rows. It emits:

- a de-identified source corpus;
- 10,000 synthesis/extraction cases;
- 10,000 embedding relevance cases; and
- 10,000 reranking candidate-list cases.

All three views share case IDs backed by committed `kb_document` artifact
citations. Labels are operational silver evidence, not human-authored gold. A
separate audited gold subset is still required for absolute-quality claims; this
suite is the canonical paired A:B and regression surface.

Run from the repository root:

```sh
python3 benchmarks/gemma4_baseline/build_254_fixtures.py
```

The builder connects to `.254` with the operator's existing SSH key, streams
read-only PostgreSQL `COPY` output through memory, deterministically redacts common
identity/credential patterns, hashes source identifiers, builds fixed BM25 hard
negatives, verifies exact counts, scans outputs, and records immutable hashes in
`benchmarks/fixtures/gemma4-unified/ab-v1/manifest.json`.

The manifest's source hashes and all emitted case/document IDs must be excluded
from training. Models, dimensions, quantizations, and rerankers are comparable only
when they consume the same manifest hashes.

Validate the frozen bundle independently:

```sh
python3 benchmarks/gemma4_baseline/validate_fixtures.py
```

## What the 10,000 cases mean

This is one paired case population exposed through three task views, not three
unrelated samples. Every `case_id` has:

- a synthesis/extraction input and expected committed-artifact payload;
- an embedding query with its cited positive document; and
- a reranking list containing that positive and 19 fixed BM25 hard negatives.

That construction makes every A:B comparison paired. It also prevents candidate
drift from making an embedding or reranking result look better merely because it
received easier negatives. The exact 10,000-case stratum counts, source snapshot
hashes, and output hashes are frozen in `manifest.json`.

The labels are de-identified operational **silver** labels derived from committed
artifacts and their document citations. They are suitable for paired model
selection, regression testing, and pre/post-training deltas. They are not a
substitute for a human-audited gold set when claiming absolute semantic quality.
All source hashes, case IDs, and document IDs in this bundle must be excluded from
training data.

## Run one model

Synthesis uses temperature 0, seed 1, a generic JSON grammar, the task-specific
schema, and the exact frozen source corpus:

```sh
python3 benchmarks/gemma4_baseline/run_synthesis_ab.py \
  --endpoint http://127.0.0.1:8920 --model MODEL --label LABEL \
  --output-dir results/LABEL --workers WORKERS
```

Embedding uses the same 10,000 queries and fixed 20-document lists. It reports
Recall@1/5/10, MRR@10, and nDCG@10 at the model's native width. A model wider than
4,000 also receives an `untrained_prefix_4000` diagnostic; that number is plainly
labelled and must not be represented as trained Matryoshka support. This runner
requires NumPy:

```sh
python3 benchmarks/gemma4_baseline/run_embedding_ab.py \
  --endpoint http://127.0.0.1:8920 --model MODEL --label LABEL \
  --output-dir results/LABEL
```

Both runners append case-level JSONL and resume by `case_id`. Summary files record
the fixture-manifest hash. Preserve raw rows: paired confidence intervals and
post-training comparisons require case-level measurements, not only averages.

Produce a deterministic paired A:B report from any two complete raw result files:

```sh
python3 benchmarks/gemma4_baseline/compare_ab.py \
  --kind embedding --left results/A/raw_embedding_A.jsonl \
  --right results/B/raw_embedding_B.jsonl --left-label A --right-label B \
  --left-summary results/A/summary_embedding_A.json \
  --right-summary results/B/summary_embedding_B.json \
  --left-environment results/pre/RUN_STATE.json \
  --right-environment results/post/RUN_STATE.json \
  --output results/compare_A_B.json
```

The comparison rejects different case populations and reports right-minus-left
deltas with 10,000-replicate paired percentile-bootstrap 95% intervals. It also
rejects different fixture hashes, hosts, kernels, GPU identities, runtime images,
or llama.cpp builds; cross-machine results are diagnostic, not qualification.

Rerankers use the frozen 20-document candidate lists through aimee's aligned-score
`POST /rerank` contract:

```sh
python3 benchmarks/gemma4_baseline/run_reranking_ab.py \
  --endpoint http://127.0.0.1:8920 --label RERANKER \
  --output-dir results/RERANKER
```

The runner applies the same model-independent input bound to every provider:
queries are at most 512 UTF-8 characters and candidates at most 1,024, retaining
two-thirds from the head and one-third from the tail with a visible truncation
marker. The `.254` controller launches both Ettin encoders with eight concurrent
requests, four pairs per request, 32 processing slots, a 65,536-token aggregate
context, a 2,048-token logical batch, and a 2,048-token physical batch. Character
bounds do not imply a 512-token bound for code and punctuation-heavy inputs. The
pinned concurrent profile saturates available execution capacity while preserving
identical evidence without provider-specific tokenizers selecting different text.
The summary records this load profile; use `--workers 1` only for a separately
labelled isolated-latency pass, not for the canonical traffic sweep.

## Reproduce the `.254` six-model baseline

`models.json` pins the artifact repository, revision, filename, byte count,
runtime image, and llama.cpp build for Gemma 4 E2B, E4B, 12B, 26B-A4B, 31B, and
Qwen 3.6 35B-A3B. The approximately-four-bit publisher artifacts are used without
requantization. E2B and E4B are Q4_0 because their publisher does not provide
Q4_K_M; treat that quantization asymmetry as a caveat for close results.

The frozen fixture manifest mechanically declares the required model/view matrix:
all six instruction bases must run synthesis and native-width embedding; they are
explicitly excluded from reranking because they are not trained cross-encoders.
Ettin-68M and Ettin-400M are both required for reranking and excluded from
synthesis/embedding. They provide the supported CPU/small and GPU/mid incumbent
controls respectively. The validator fails if either control or any matrix cell
changes silently.

Download and verify the artifacts:

```sh
python3 benchmarks/gemma4_baseline/download_models.py \
  --output-dir /mnt/media/gemma4-baseline/models \
  --reuse-26b /mnt/media/.plugins/aimee-llm/llm/models/mid/synth.gguf
```

On `.254`, the controller runs one model and one role at a time against the RX
7900 XTX. It first records isolated Ettin-68M CPU and Ettin-400M GPU controls,
then records artifact SHA-256 values and environment identity, pauses the
deployed LLM container so production cannot share VRAM with the measurement,
runs the full 10,000 synthesis and 10,000 embedding cases, and restores the exact
production container in a `finally` block:

Synthesis concurrency is an explicit per-model load profile rather than a fixed
two-request cap. E2B uses 64 client workers and 64 server slots with a 131,072-token
aggregate context (2,048 tokens per slot). Progress state and hardware artifacts
record the selected profile. Larger models use progressively smaller profiles to
leave room for their weights on the 24 GiB card. E2B embedding batches contain 64
inputs and run against 64 server slots with 2,048 context tokens available to
each input instead of being serialized through one slot.

Model benchmark servers bound llama.cpp's prompt cache to 512 MiB instead of its
8 GiB default. This preserves reuse of common instruction, schema, and chat-template
prefixes without allowing cache policy to dominate the card. The frozen corpus is
at most 6,229 characters per document, and observed E2B synthesis traffic is at
most 1,557 total tokens, so allocating a large training-length context to every
slot would consume VRAM without representing this workload. Context capacity,
prompt-cache capacity, and slot count are tuned independently.

E4B also starts at 64 slots. The 12B profile starts at 32, 26B at 16, 31B at 8,
and Qwen 3.6 35B-A3B at 4. Before a complete 12B-or-larger view, run a short
all-slots-occupied capacity check using the same frozen cases. Reduce a profile
only when server loading or concurrent requests actually fail; do not infer a
small slot count merely from parameter count.

```sh
python3 benchmarks/gemma4_baseline/run_254_sweep.py
```

`--skip-ettin` skips both controls. A normal or resumed qualification run requires
both controls over all 10,000 frozen reranking cases.

After a complete restored sweep, generate all 15 six-model pairs for both views
(30 paired reports total):

```sh
python3 benchmarks/gemma4_baseline/pairwise_reports.py \
  --results /mnt/media/gemma4-baseline/results \
  --models /mnt/media/gemma4-baseline/models.json
```

`--max-cases N` is only an integration-smoke option and is never a qualification
result. The canonical baseline is exactly the 10,000 cases named by the frozen
manifest for every model. Replacement rerankers consume `reranking.jsonl` through
their provider adapter, but must retain the identical candidate order, case IDs,
and input-bound algorithm so paired deltas against Ettin remain meaningful.

Use `--modes synthesis` for a synthesis-only recovery. This avoids revisiting a
completed embedding run and replacing its measured request telemetry with zeros
from a cache-only pass. For example, the isolated E4B retry is:

```sh
python3 benchmarks/gemma4_baseline/run_254_sweep.py \
  --skip-ettin --labels gemma4_e4b --modes synthesis
```

## EuroBERT reranker extension

The official EuroBERT-210M and EuroBERT-610M checkpoints are masked-language
encoder bases, not ready-made rerankers. `eurobert_rerankers.json` therefore pins
both official base revisions and gives each one the same one-score cross-encoder
head and training recipe. Each model sees the same ordered 576,000-example subset
of the pinned `cross-encoder/ettin-reranker-v1-data` teacher-score dataset: 72,000
examples from each of eight declared configurations, MSE loss, 512 tokens, one
epoch, effective batch 16, seed 12, and bf16. No frozen evaluation row is used for
training.

This is a bounded matched EuroBERT comparison, not a claim that the training
budgets are equal to the released Ettin models: Ettin was trained on the full
143,393,475-example dataset. Report that asymmetry next to every EuroBERT/Ettin
delta. The extension deliberately does not alter the frozen fixture manifest, so
its SHA-256 and all already-completed Ettin/Gemma results remain valid.

The `.254` extension controller builds a manifest-pinned PyTorch/ROCm image,
requires each model to pass one bf16 forward/backward/AdamW step at the declared
batch size and 512-token limit, trains or resumes each model, serves aligned
`/rerank` scores with a 32-pair microbatcher, runs the same 10,000 cases and
8-worker/4-pair load profile, and restores production in a `finally` block. It
returns success only after the production health endpoint is ready. The
controller takes the same `sweep.lock` as the main controller and therefore
cannot overlap another GPU benchmark:

Training completion fails closed: a saved `config.json` alone is not accepted.
The controller requires a completed provenance record and rechecks the byte size
and SHA-256 of every final model artifact before serving or skipping training.

```sh
python3 benchmarks/gemma4_baseline/run_254_eurobert_rerankers.py
```

It may be queued safely behind an active main sweep with `--wait-for-lock`. The
main controller restores production before process exit releases the lock; the
EuroBERT controller acquires it only afterward and independently preserves that
restoration state in its own `finally` block. After acquiring the lock it also
fails closed unless the prior main `RUN_STATE.json` records both completion and
successful production restoration. Use `--handoff-state` to override that state
path when the preceding sweep used a nondefault results directory.

Use `--max-cases N` only to smoke-test the runtime. Model, dataset, container,
Python-package, training, and serving identities are recorded separately from
the unchanged evaluation-suite identity.

After both EuroBERT rerankers complete and production is restored, the full
controller generates and verifies all six paired comparisons across Ettin
68M/400M and EuroBERT 210M/610M. The following command reproduces them manually:

```sh
python3 benchmarks/gemma4_baseline/reranker_pairwise_reports.py \
  --results /mnt/media/gemma4-baseline/results \
  --manifest /mnt/media/gemma4-baseline/repo/benchmarks/gemma4_baseline/eurobert_rerankers.json \
  --main-state /mnt/media/gemma4-baseline/results/RUN_STATE.json \
  --eurobert-state /mnt/media/gemma4-baseline/eurobert_sweep_state.json
```

Every cross-family report states the unequal training budgets. Reranking latency
deltas remain diagnostic until Ettin is rerun from an empty output directory
under one clean load profile; paired quality deltas remain valid.

The remaining qualification stages can be queued as one resumable, fail-closed
chain. It waits at the EuroBERT handoff gate, then runs the E4B synthesis-only
recovery, followed by both required views for Gemma 4 26B-A4B, Gemma 4 31B, and
Qwen3.6 35B-A3B. A nonzero child exit stops the chain; every child controller
must restore a healthy production service before returning success. The parent
also performs an independent final production-health probe before recording the
chain complete:

```sh
python3 benchmarks/gemma4_baseline/run_254_remaining_chain.py
```

Run the contract tests (exact counts/hashes/shared IDs, matrix enforcement,
fail-closed secret scanning, reranker bounds, and resume behavior):

```sh
python3 -m unittest benchmarks.tests.test_gemma4_baseline_contract -v
```

Before publishing any completed view, generate fail-closed acceptance evidence:

```sh
python3 benchmarks/gemma4_baseline/validate_result_checkpoint.py \
  --bundle benchmarks/fixtures/gemma4-unified/ab-v1 \
  --result-dir /mnt/media/gemma4-baseline/results/gemma4_12b \
  --label gemma4_12b --view embedding
```

The validator enforces the exact frozen case population, latest-row success,
recomputed metrics, suite identity, positive cold-load telemetry, sane load/run
hardware snapshots, and a secret scan.
Full controller runs invoke it automatically and persist `validation_<view>.json`
before marking a view complete; `--max-cases` smoke runs intentionally skip the
10,000-case acceptance gate.
