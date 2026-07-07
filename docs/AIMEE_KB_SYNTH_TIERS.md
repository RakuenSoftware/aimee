# aimee-kb image synth tiers

The `aimee-kb-*` image is the unified Vulkan llama.cpp stack: one runtime serving
**embeddings** (`/embed`), **reranking** (`/rerank`), and **synthesis**
(`/v1/chat/completions`) from baked-in GGUFs. Four published tiers differ only in the
**synth model** (and its runtime profile); embed + rerank are identical within a
CPU/GPU class, so a KB stays byte-portable when you swap the GPU synth tier.

| Image | Embed / Rerank | Synth | Runtime (target card) | Pull size |
|---|---|---|---|---|
| `aimee-kb-cpu` | Qwen3-Emb-0.6B / ettin-68m (1024-dim) | gemma-4-E4B (**Tier A only**) | CPU (`NGL=0`) | ~6.5 GB |
| `aimee-kb-gpu-small` | Qwen3-Emb-4B / ettin-400m (2560-dim) | **Gemma 4 12B** `qat-UD-Q4_K_XL` | GPU, dense, FA+K8V4 — 16 GB | ~11.4 GB |
| `aimee-kb-gpu-mid` | Qwen3-Emb-4B / ettin-400m (2560-dim) | **Gemma 4 26B-A4B** `qat-UD-Q4_K_XL` | GPU, MoE (4B active), FA+K8V4, fully resident, 2 slots — 24 GB | ~17.8 GB |
| `aimee-kb-gpu-large` | Qwen3-Emb-4B / ettin-400m (2560-dim) | **Gemma 4 26B-A4B** `qat-UD-Q4_K_XL` | Same GGUF as `gpu-mid`, 4 slots — 32 GB | ~17.8 GB |

`gpu-large` is byte-identical to `gpu-mid` (same synth GGUF/revision/SHA + MoE/FA profile);
it only bakes `SYNTH_SLOTS=4` instead of `2`, so a 32 GB card runs **4 concurrent agents**
each with the model's full native **256 K** window (deploy `AIMEE_LLM_SYNTH_CTX=1048576`).
Gemma-4 uses interleaved sliding-window attention (only 5 of 30 layers are full-attention),
so even 4×256 K KV stays ~28.5 GiB — validated against the 24 GB `gpu-mid` card (2×128 K =
22.4/24 GiB). `gpu-small`, `gpu-mid`, and `gpu-large` share the **same embedder + reranker**
(2560-dim), so a KB embedded on one is byte-compatible with the others — switching the synth
tier is a plugin **image swap**, no re-embed.

**Image size** (amd64, compressed pull from ghcr): `cpu` ~6.5 GB, `gpu-small` ~11.4 GB,
`gpu-mid` / `gpu-large` **~17.8 GB**. The mid/large image bakes ~19.3 GB of GGUFs
(embed 4.28 GB + rerank 0.79 GB + synth 14.25 GB) → ~20 GB uncompressed on disk. `gpu-large`
carries the *same* layers as `gpu-mid` (only the `SYNTH_SLOTS=4` env differs), so it is the
same size — allow ~20 GB free disk on the host per GPU image, plus transient build headroom.

## Tier-A vs Tier-B synthesis

The curator synthesizes in two tiers:

- **Tier A**: mechanical extract/index passes (extract docs, chunk, tag, index).
- **Tier B**, the reasoning passes: judge, resolve entities, reconcile contradictions,
  synthesize, promote.

`aimee-kb-cpu` runs **Tier A only**. Its gemma-4-E4B synth is not wired as a Tier-B provider (a weak model would poison the graph), so a CPU-only deployment gets retrieval + Tier-A
synthesis and nothing more. **Tier B needs a GPU tier (`gpu-small` / `gpu-mid` / `gpu-large`)
or an external LLM.** With no Tier-B provider the reasoning passes are skipped, not errored. Full
config surface: [KB_LLM_BACKENDS.md](KB_LLM_BACKENDS.md).

## Package names

Two image families share the `aimee-kb` prefix. Keep them straight:

- **`aimee-kb`** (no suffix): the KB service (DB2 + curator). Runs no model; it calls one
  over HTTP. See [KB_LLM_BACKENDS.md](KB_LLM_BACKENDS.md).
- **`aimee-kb-{cpu,gpu-small,gpu-mid,gpu-large}`**: the inference tiers in this doc (embed +
  rerank + synth), built from `Dockerfile.aimee-llm`, deployed as the SmoothNAS `aimee-llm` plugin.

Retired names: `aimee-llm-cpu`/`aimee-llm-gpu` were the old tier names (now
`aimee-kb-cpu`/`aimee-kb-gpu-small`); `aimee-embedder`/`aimee-embedder-4b` were the
standalone torch embedder, now baked into the tiers.

## Deploy: one plugin image swap

The synth tier is chosen by which image the SmoothNAS `aimee-llm` plugin references,
e.g. `ghcr.io/rakuensoftware/aimee-kb-gpu-mid:latest` for Gemma 4 26B-A4B. No separate synth
container, no `SYNTH_LOCAL` juggling: the tier *is* the synth.

### Two consumers of the gateway synth (`/v1/chat/completions`)

The gateway (`http://<runtime-gw>:8742/v1`, model alias `aimee-synth`) is used by two
independent callers, one automatic, one an explicit operator step:

1. **KB curator synthesis (automatic).** The `aimee-kb` plugin points `LLM_ENDPOINT` /
   `LLM_MODEL` at the gateway, so the curator's `tier_a`/`tier_b` use the synth tier the
   moment the image is deployed. Swapping tiers needs no KB change.

2. **aimee-server delegate, an explicit runtime registration (NOT automatic).** To make
   the local synth usable as an `aimee` delegate (roundtable / fallback), register it once
   against the server (endpoint is model-neutral, so the registration survives synth-tier
   swaps):

   ```sh
   aimee agent local local-synth http://<runtime-gw>:8742/v1 \
       --model aimee-synth --provider openai --slots <SYNTH_SLOTS>
   ```

   The gateway runs auth-off on the internal bridge, so the agent uses `auth_type: none`.
   This lives in the server's `agents.json` (runtime/per-deployment state, not baked into
   the image). On `.254` this is registered as `local-synth` and sits in `fallback_chain`.

## Synth runtime knobs (baked per tier; overridable via plugin env)

| Env | Default (per tier) | Meaning |
|---|---|---|
| `AIMEE_LLM_SYNTH_CTX` | 32768 baked; gpu-mid deploys `262144` (2 × 128 K), gpu-large `1048576` (4 × 256 K) | synth context window (total across slots) |
| `AIMEE_LLM_SYNTH_SLOTS` | 1 (2 on gpu-mid, 4 on gpu-large) | `--parallel` concurrent slots |
| `AIMEE_LLM_SYNTH_FA` | `off` cpu / `on` gpu | flash-attention (required for quantized V-cache) |
| `AIMEE_LLM_SYNTH_KV_K` / `_KV_V` | `q8_0` / `q4_0` (K8V4) | KV cache quant on the gpu tiers |
| `AIMEE_LLM_SYNTH_MOE` | `1` on gpu-mid | enable the `--n-cpu-moe` expert-offload knob |
| `AIMEE_LLM_SYNTH_N_CPU_MOE` | `0` on gpu-mid | MoE layers whose experts live in system RAM (0 = fully GPU-resident) |

### Tuning `N_CPU_MOE` on gpu-mid (Gemma 4 26B-A4B, ~14 GB synth)

The mid tier is **Gemma 4 26B-A4B**, a MoE with only **4B active parameters/token** at
`qat-UD-Q4_K_XL` (~14 GB). Unlike a 22 GB synth, it **co-fits embed+rerank (~7 GB) on a
24 GB card fully resident** (~21 GB + Gemma's cheap sliding-window KV), so the default is
`N_CPU_MOE=0`: no expert offload, fully GPU-resident.

| Card | Suggested `N_CPU_MOE` | Notes |
|---|---|---|
| 24 GB | `0` (default) | fully resident; fastest. Drop `SYNTH_CTX` or offload a few layers only if VRAM is tight with 2 slots × 128 K |
| 16 GB | ~24–32 | ~14 GB synth won't co-fit ~7 GB retrieval; spill most experts to RAM (slow but works) |

Raise `N_CPU_MOE` only if the container logs a VRAM allocation failure; each offloaded
layer frees GPU memory at the cost of RAM-path latency. Only 4B params are active, so
offload costs less here than on a dense synth of the same size.

### Flash-attention / K8V4

K8V4 (`q8_0` K / `q4_0` V) is verified working on RADV/gfx1100 (7900 XTX). FA is
enforced structurally (llama.cpp refuses a quantized V-cache without it), so a
healthy container proves FA engaged. If a backend genuinely can't do FA, set
`AIMEE_LLM_SYNTH_KV_V=f16` (K8V8 does **not** help; any quantized V needs FA).

### GPU runtime: Mesa 25 / cooperative matrix

The GPU tiers are based on `debian:trixie-slim` (Mesa 25) on purpose: its RADV exposes
`VK_KHR_cooperative_matrix` on RDNA3 (gfx1100), so llama.cpp uses the card's WMMA matrix
cores. On an older Mesa without coopmat (bookworm's 22.3.6) llama.cpp falls back to scalar
shaders. The synth goes compute-bound, the memory bus sits idle, and generation crawls.

Measured on the 7900 XTX, gpu-mid resident: **~1265 tok/s prompt, ~62–95 tok/s generation**
with coopmat; ~33–76 / ~30 without it. If a GPU tier is slow, check the base image is
trixie (Mesa ≥ 24.1) and that `mem_busy` climbs under load. A pegged GPU with an idle
memory bus means the matrix cores aren't in play.
