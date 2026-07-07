# Runbook: cut over to the unified `aimee-llm` container

The unified-llm-container migration (P7). **Operator-gated**: the production flip
(deploy + corpus re-embed) is a deliberate maintenance op, never automatic. P1–P6 ship the
pieces; this runbook is the order of operations + the gates.

Validated before this runbook (`.253`/`.254`, see `benchmarks/results/unified-llm/`):
the images build, serve `/embed`+`/rerank`(ettin encoder + gateway
head)+`/v1/chat/completions`, and offload to the AMD 7900 XTX via Vulkan (`-ngl 99`).

> **Naming update (post-cutover):** the images built here were renamed
> `aimee-llm-cpu`/`aimee-llm-gpu` → `aimee-kb-cpu`/`aimee-kb-gpu-small`, and a third GPU
> tier `aimee-kb-gpu-mid` (Gemma 4 26B-A4B) was added; `aimee-llm` is now only the plugin
> name. The steps below keep the original names as a record, for the current tiers and
> build-args see [../AIMEE_KB_SYNTH_TIERS.md](../AIMEE_KB_SYNTH_TIERS.md).

## 0. What changes

- **From:** torch `aimee-embedder` (pplx-embed + ettin via sentence-transformers) + the
  stock `llm` llama.cpp container (gemma synth).
- **To:** one `aimee-llm` image (CPU or GPU tier): Vulkan llama.cpp serving embed
  (Qwen3-Embedding) + rerank (ettin encoder + the gateway Dense head) + synth (gemma),
  models baked in.
- **Corpus re-embed required:** `pplx-embed` → `Qwen3-Embedding` is a **`model_id` change**
  (and 0.6B→1024 / 4B→2560 a possible **dim** change), so the `kb_meta` drift guard (P1,
  now activated, `db2_set_embedder_model_id`) will **refuse** to serve the new embedder
  against the old corpus. The re-embed is the controlled drop-and-rebuild below.

## 1. Pre-cutover ship-floor gate (staging, BEFORE the production flip)

The ship-floor gate is a **pre-cutover precondition** (proposal §2): the new
embed+rerank+fusion stack must hit **≥95%** of the pplx/ettin baseline (nDCG@10 / MRR@10 +
top-k rank-agreement ≥0.95) on the real corpus, in staging, against the checked-in fixture
`tests/fixtures/pplx_baseline.json`. **Gate fail → freeze, investigate, do NOT cut over.**
Because the gate is upstream of the cutover, pplx/ettin is removed *completely* in the
release (no in-image rollback flag); a post-cutover revert is a deploy-tag rollback (§5).

## 2. Build + publish the images

CI builds + publishes both tiers as part of the normal release cycle:
- **main:** `.github/workflows/publish-images.yml` (via `auto-release.yml`) builds
  `aimee-kb-cpu` + `aimee-kb-gpu-small` + `aimee-kb-gpu-mid` on every release and tags them
  `:<version>` + `:latest`. They are in both the `build` and the `merge` matrices; the
  merge step applies the tags, so a build that pushes only by digest leaves `tags:null`.
  (`gpu-mid` is amd64-only.)
- **testing:** `.github/workflows/publish-llm-testing.yml` builds `cpu` + `gpu-small` on
  `testing` pushes that touch `Dockerfile.aimee-llm` or the gateway/supervisor scripts,
  tagged `:testing` (+ `:testing-<sha>`, amd64). `gpu-mid` is dispatch-only
  (`build_kb_gpu_mid=true`): a tested-but-unreleased image for `.254`.

To build out-of-band (e.g. on a PVE CT with docker):

```
docker build -f Dockerfile.aimee-llm -t aimee-kb-cpu .
docker build -f Dockerfile.aimee-llm -t aimee-kb-gpu-small \
  --build-arg EMBED_REPO=Qwen/Qwen3-Embedding-4B-GGUF \
  --build-arg EMBED_FILE=Qwen3-Embedding-4B-Q8_0.gguf \
  --build-arg RERANK_REPO=cross-encoder/ettin-reranker-400m-v1 \
  --build-arg SYNTH_REPO=unsloth/gemma-4-12B-it-qat-GGUF \
  --build-arg SYNTH_FILE=gemma-4-12B-it-qat-UD-Q4_K_XL.gguf \
  --build-arg SYNTH_FA=on .
```
(gpu-mid swaps the synth args to Gemma 4 26B-A4B; see the tiers doc for the exact pins.)

## 3. Deploy to `.254` (tierd / LXC, GPU)

`.254` runs containers via **tierd** (LXC), not raw docker, with GPU passthrough via the
Vulkan/RADV render path (**not** ROCm/`kfd`). Deploy the GPU image as a tierd plugin
(manifest-swap, per the `.254` deploy memory) + `AIMEE_LLM_NGL=99`. The in-repo manifest is
`deploy/smoothnas/aimee-llm.plugin.yaml` (pin its image to `:testing` for staging,
`:latest`/`:<version>` for production). GPU passthrough uses the **built-in `gpu-amd`
profile** (compiled into tierd, the one Wolf/Steam use), so there is no custom profile to
install and no node path to pin; verified on .254 the container enumerates `Vulkan0 : AMD
Radeon Graphics (RADV GFX1100)` and offloads to VRAM. The gateway is host-published on
**:8742** (NOT :8080, since Wolf's WolfLeash UI host-publishes 8080, and two plugins on one host
port silently collide: the runtime DNATs both, one wins and the other is unreachable though
"running"). Point the kb at it: `AIMEE_EMBEDDER_URL=http://10.100.0.1:8742` (the gateway preserves the
`/embed`+`/embed_batch`+`/rerank` contract, so `embed-remote.py`/`rerank-remote.py` are
untouched). Set the kb's `embedding_model` to the new identity (activates the P1 guard) and
`embedding_dim` to the tier's dim (2560 for the 4B GPU tier). **Verify the URL actually
serves the named model before re-embedding**: a misrouted `AIMEE_EMBEDDER_URL` (still
pointing at the old embedder) would silently embed under the new identity and the guard
would not catch it (it checks identity-vs-corpus, not URL-vs-identity). **Auth on the embed
path:** the kb embeds/reranks with NO bearer (`memory_embed_http_post` sends a NULL auth
header), so when this gateway backs `AIMEE_EMBEDDER_URL` it MUST run **auth-off**: leave
`AIMEE_LLM_AUTH_TOKEN` empty/unset. The gateway treats an empty token as "auth disabled,"
acceptable here because it is `expose:false` (internal bridge only; deployment network is the
boundary). Only set `AIMEE_LLM_AUTH_TOKEN` when the gateway serves *only* bearer-capable
callers (e.g. curator `tier_b` synth); if so it comes from the secret store (vault / Docker
secret / `.gitignore`d per-host `.env`), **never a checked-in plaintext value**.

## 4. Corpus re-embed (controlled drop-and-rebuild, the `maintenance` window)

In-place `halfvec` type changes against a live kb are forbidden. The migration:

1. **Snapshot** the current vector tables.
2. Gate the kb to **`maintenance`**.
3. **Drop** the dim-sized `halfvec` tables; **recreate** at the new `(model_id, dim)` (the
   schema applies at the new dim; `db2_embedding_model_record_or_check` records the new
   identity on the fresh `kb_meta`).
4. **Replay** the source rows through the new embedder (bounded batch).
5. **Parity gate:** pre-drop vs post-backfill counts must match **and** a sampled
   value-check must pass (row counts alone miss silent corruption: wrong rows re-embedded,
   off-by-one batch, wrong prompt template, a dim swap). Over a held-out sample (≥1% or 10k
   rows, whichever is larger): every vector has the expected `array_length`, no NaN/all-zero
   rows, and, where a stable subset is re-embeddable from the snapshot, cosine vs the
   snapshot is within tolerance for the *same* model (and `kb_meta.schema_embedder_model_id`
   is the new identity). Any divergence → `degraded`, hold. On parity → lift `maintenance`.

Plan the window from the corpus size × the new embedder's throughput (record peak RSS per
inner `llama-server`; the embed model is offloaded so it's GPU-bound).

## 5. Rollback (one-time safety net, initial cutover only)

The ship-floor gate (§1) means a regression never ships. If a post-cutover issue still
demands it: deploy-tag **rollback to the prior release tag** (which still contains
pplx/ettin) **+** the **reverse re-embed** (a checked-in, idempotent schema-revert migration
restoring the prior dim-sized `halfvec` from the §4 snapshot, CI round-tripped before
cutover). This is a gated maintenance op, not an instant toggle. Once the first
post-cutover release is validated, no prior tag contains pplx/ettin and rollback is no
longer deploy-tag-based; retain the prior tag in the registry for a defined window (e.g. 6
months). **Snapshot retention ≥ tag retention**: the §4 snapshot is the only rollback
input once the tag ages out, so it must outlive the tag, with a checksum and an expiry
review. Pre-cutover checklist (all must be true before §3): snapshot present + checksum
verified; reverse-migration **rehearsed in staging** (round-trips to the prior dim/identity);
ship-floor gate green.

## 6. Retire the old surfaces

After validation: remove `Dockerfile.embedder` + the `embedder`/`llm` compose/deploy
services + the `codex-auth`/pplx-ettin references. (Kept until here so the prior tag is a
working rollback target, §5.)

## Gates summary

| Gate | When | Pass |
|------|------|------|
| ship-floor ≥95% | staging, pre-cutover | nDCG/MRR ≥95% of pplx fixture + rank-agreement ≥0.95 |
| drift guard | kb startup post-deploy | `kb_meta` records the new `(model_id, dim)`; refuses a stale mismatch |
| parity | post-re-embed | pre-drop == post-backfill counts; else `degraded` |
| kill-a-child smoke | post-deploy | one inner llama-server killed; others keep serving |
