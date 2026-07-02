# aimee-kb image synth tiers

The `aimee-kb-*` image is the unified Vulkan llama.cpp stack — one runtime serving
**embeddings** (`/embed`), **reranking** (`/rerank`), and **synthesis**
(`/v1/chat/completions`) from baked-in GGUFs. Three published tiers differ only in the
**synth model** (and its runtime profile); embed + rerank are identical within a
CPU/GPU class, so a KB stays byte-portable when you swap the GPU synth tier.

| Image | Embed / Rerank | Synth | Runtime |
|---|---|---|---|
| `aimee-kb-cpu` | Qwen3-Emb-0.6B / ettin-68m (1024-dim) | gemma-4-E4B | CPU (`NGL=0`) |
| `aimee-kb-gpu-small` | Qwen3-Emb-4B / ettin-400m (2560-dim) | **Gemma 4 12B** `qat-UD-Q4_K_XL` | GPU, dense, FA+K8V4 |
| `aimee-kb-gpu-mid` | Qwen3-Emb-4B / ettin-400m (2560-dim) | **Gemma 4 26B-A4B** `qat-UD-Q4_K_XL` | GPU, MoE (4B active), FA+K8V4, fully resident |

`gpu-small` and `gpu-mid` share the **same embedder + reranker** (2560-dim), so a KB
embedded on one is byte-compatible with the other — switching the synth tier is a
plugin **image swap**, no re-embed.

## Deploy: one plugin image swap

The synth tier is chosen by which image the SmoothNAS `aimee-llm` plugin references —
e.g. `ghcr.io/rakuensoftware/aimee-kb-gpu-mid:latest` for Gemma 4 26B-A4B. No separate delegate
container, no `SYNTH_LOCAL` juggling: the tier *is* the synth. The gateway's
`/v1/chat/completions` is what the server/curator use, so the delegate wiring is
automatic once the image is deployed.

## Synth runtime knobs (baked per tier; overridable via plugin env)

| Env | Default (per tier) | Meaning |
|---|---|---|
| `AIMEE_LLM_SYNTH_CTX` | 32768 (raise on GPU) | synth context window |
| `AIMEE_LLM_SYNTH_SLOTS` | 1 (2 on gpu-mid) | `--parallel` concurrent slots |
| `AIMEE_LLM_SYNTH_FA` | `off` cpu / `on` gpu | flash-attention (required for quantized V-cache) |
| `AIMEE_LLM_SYNTH_KV_K` / `_KV_V` | `q8_0` / `q4_0` (K8V4) | KV cache quant on the gpu tiers |
| `AIMEE_LLM_SYNTH_MOE` | `1` on gpu-mid | enable the `--n-cpu-moe` expert-offload knob |
| `AIMEE_LLM_SYNTH_N_CPU_MOE` | `0` on gpu-mid | MoE layers whose experts live in system RAM (0 = fully GPU-resident) |

### Tuning `N_CPU_MOE` on gpu-mid (Gemma 4 26B-A4B, ~14 GB synth)

The mid tier is **Gemma 4 26B-A4B** — a MoE with only **4B active parameters/token** at
`qat-UD-Q4_K_XL` (~14 GB). Unlike a 22 GB synth, it **co-fits embed+rerank (~7 GB) on a
24 GB card fully resident** (~21 GB + Gemma's cheap sliding-window KV), so the default is
`N_CPU_MOE=0` — **no expert offload, no CPU/RAM path in the hot loop.** That is the whole
point of this tier vs. a larger dense/MoE synth: it stays on the GPU and runs fast.

| Card | Suggested `N_CPU_MOE` | Notes |
|---|---|---|
| 24 GB | `0` (default) | fully resident; fastest. Drop `SYNTH_CTX` or offload a few layers only if VRAM is tight with 2 slots × 128 K |
| 16 GB | ~24–32 | ~14 GB synth won't co-fit ~7 GB retrieval; spill most experts to RAM (slow but works) |

Raise `N_CPU_MOE` only if the container logs a VRAM allocation failure; each offloaded
layer frees GPU memory at the cost of RAM-path latency. Because only 4B params are active,
even a modest offload stays far faster than a 22 GB synth with the same offload.

> **Why Gemma 4 26B-A4B over Qwen 3.6 35B-A3B here:** the 22 GB Qwen synth could not
> co-fit the 7 GB retrieval stack on a 24 GB card, forcing ~half its MoE experts onto the
> CPU/RAM path (`N_CPU_MOE≈20`) → ~14 tok/s generation regardless of the GPU. Gemma 4
> 26B-A4B (~14 GB, 4B active) fits resident, eliminating the offload bottleneck.

### Flash-attention / K8V4

K8V4 (`q8_0` K / `q4_0` V) is verified working on RADV/gfx1100 (7900 XTX). FA is
enforced structurally — llama.cpp refuses a quantized V-cache without it — so a
healthy container proves FA engaged. If a backend genuinely can't do FA, set
`AIMEE_LLM_SYNTH_KV_V=f16` (K8V8 does **not** help; any quantized V needs FA).
