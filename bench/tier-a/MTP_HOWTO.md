# MTP on gemma-4 with llama.cpp — what another session needs

## The invocation

```bash
llama-server \
  -hf  unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL \
  -hfd unsloth/gemma-4-E4B-it-GGUF \
  --host 0.0.0.0 --port 8110 -c 8192 --no-webui --no-mmproj -ngl 99
```

`-hfd` is the **draft repo**, same repo as the model. No `-md`. Confirm it worked:

```bash
curl -s localhost:8110/slots | python3 -c "import json,sys;print(json.load(sys.stdin)[0]['speculative'])"
# must print True
```

If that prints `False`, speculation is off and the run is just a normal run.

## Why the obvious way fails

`-m model.gguf -md mtp-head.gguf` **cannot work**, for three compounding reasons:

1. `--mtp` is registered for `LLAMA_EXAMPLE_DOWNLOAD` only, so `llama-server`
   has no such flag and it never appears in `--help`.
2. The speculative type is inferred from the **download plan**, not the model
   file (`common/arg.cpp:549`).
3. An explicit draft file **actively suppresses** that inference
   (`common/arg.cpp:543`): *"an explicit draft file selection (e.g. -md with
   -hfd) disables the sidecar resolution of the draft repo"* → `plan_spec.mtp = {}`.

Sidecar discovery only runs when a draft **repo** is set, because `plan_spec` is
built from `params.speculative.draft.mparams` (`arg.cpp:398`).

Symptom of getting it wrong: the head loads, the log says
`[spec] failed to measure draft model memory: failed to create llama_context from
model`, and `/slots` reports `speculative: false`.

## What it does and does not buy

| | measured |
|---|---|
| speedup, E4B UD-Q4 | **1.83x** (22.9 → 41.9 notes/min) |
| speedup, E2B UD-Q4 | **1.59x** (27.0 → 43.0 notes/min) |
| identical to a sequential run | **no** — 74/100 notes |
| identical to another MTP run | **yes** — 100/100, on E4B and E2B |

Speedup is **model-dependent**: speculation reclaims idle compute, and a smaller
model is less bandwidth-bound at batch=1, so there is less to reclaim. Do not
quote one number for "MTP".

It is **not** output-identical to sequential, despite greedy decoding and exact
verification. Verification feeds several tokens per forward pass, which changes
the target's batch shape, which changes float reduction order, which flips
near-ties. Same root cause as parallel slots.

It **is** repeatable, which is what a benchmark needs: hold the configuration
fixed across every arm and record it with the results. Arms run under MTP are
comparable to each other and NOT to arms run sequentially.

## Do not combine with parallel slots

`-np 32` (with or without MTP) is **not repeatable**: two runs of the identical
config agreed on 63/100 raw completions and 75/100 extracted facts, and wall time
varied 71 s vs 61 s. With many requests in flight the batch composition follows
arrival timing, which is not reproducible. Root-cause isolation is in
`harness/investigate_np32_nondeterminism.sh` (and the XTX variant).

Whether MTP additionally helps or hurts at 32 slots is **unmeasured** — the
4.34x/4.54x comparison that suggested a slowdown came from different sample
sizes and is inside the 16% run-to-run noise. `harness/mtp_speed_matrix_xtx.sh`
runs the 2x2 with three repeats per cell to settle it.

## The other determinism trap

Greedy on this stack is bit-reproducible **only from a freshly started server**:

| | identical |
|---|---:|
| banked arm vs fresh run | 20/20 (three independent restarts) |
| banked arm vs run against a WARM server | 14/20 |

llama.cpp reuses a cached prompt prefix per slot; every request here shares the
same ~600-token system prompt, so recompute-vs-reuse changes the logits. **Every
arm must start from a restarted server**, and a "just re-run a few notes"
spot-check against a warm server will manufacture phantom disagreements.

## Environment

- llama.cpp `b10201-9-g0005475` (2026-07-31), CUDA build, CT 140 on `.253`
- gemma-4 MTP is present in this build but **not** as `graph_mtp` — it is
  `src/models/gemma4-assistant.cpp`, a paired target/draft arrangement reading
  the target's hidden states via `llama_get_embeddings_nextn_ith()`. Grepping for
  `graph_mtp` symbols will wrongly conclude gemma-4 is unsupported.
- The resolved head is `mtp-gemma-4-E4B-it.gguf`, arch `gemma4-assistant`, ~94 MiB.
- `-hf` downloads to the HF cache; set `HF_HOME=/opt/hf` to reuse the store.
- On `.254`, `--device Vulkan1` is **mandatory** — Vulkan0 is a 16 GB Phoenix
  iGPU and llama.cpp takes the first device by default (defect 30).
