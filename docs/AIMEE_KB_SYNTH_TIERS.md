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
| `aimee-kb-gpu-mid` | Qwen3-Emb-4B / ettin-400m (2560-dim) | **Qwen 3.6 35B-A3B** `UD-Q4_K_XL` | GPU, MoE, FA+K8V4 + `--n-cpu-moe` |

`gpu-small` and `gpu-mid` share the **same embedder + reranker** (2560-dim), so a KB
embedded on one is byte-compatible with the other — switching the synth tier is a
plugin **image swap**, no re-embed.

## Deploy: one plugin image swap

The synth tier is chosen by which image the SmoothNAS `aimee-llm` plugin references —
e.g. `ghcr.io/rakuensoftware/aimee-kb-gpu-mid:latest` for Qwen. No separate delegate
container, no `SYNTH_LOCAL` juggling: the tier *is* the synth. The gateway's
`/v1/chat/completions` is what the server/curator use, so the delegate wiring is
automatic once the image is deployed.

## Synth runtime knobs (baked per tier; overridable via plugin env)

| Env | Default (per tier) | Meaning |
|---|---|---|
| `AIMEE_LLM_SYNTH_CTX` | 32768 (raise on GPU) | synth context window |
| `AIMEE_LLM_SYNTH_SLOTS` | 1 | `--parallel` concurrent slots |
| `AIMEE_LLM_SYNTH_FA` | `off` cpu / `on` gpu | flash-attention (required for quantized V-cache) |
| `AIMEE_LLM_SYNTH_KV_K` / `_KV_V` | `q8_0` / `q4_0` (K8V4) | KV cache quant on the gpu tiers |
| `AIMEE_LLM_SYNTH_MOE` | `1` on gpu-mid | enable MoE expert-offload |
| `AIMEE_LLM_SYNTH_N_CPU_MOE` | ~20 on gpu-mid | **the tune knob** — MoE layers whose experts live in system RAM (clamped `[0,40]`) |

### Tuning `N_CPU_MOE` on gpu-mid (Qwen, 22 GB synth)

On a 24 GB card the 22 GB synth can't fully co-fit embed+rerank (~5.5 GB), so some
experts spill to RAM. Higher `N` = less VRAM, more RAM traffic, slower. Because the
mid image bakes Qwen *as the synth* (no gemma alongside), Qwen gets ~18.5 GB of the
card → most experts stay resident:

| Card | Suggested `N_CPU_MOE` | Notes |
|---|---|---|
| 24 GB | ~20 (default) | ~half experts resident; safe margin |
| 32 GB | ~4–8 | nearly all resident, fastest |

Free system RAM needed ≈ `N/40 × 22 GB`. Raise `N` if the container logs an
allocation failure; lower it (or drop `SYNTH_CTX`) for more speed.

### Flash-attention / K8V4

K8V4 (`q8_0` K / `q4_0` V) is verified working on RADV/gfx1100 (7900 XTX). FA is
enforced structurally — llama.cpp refuses a quantized V-cache without it — so a
healthy container proves FA engaged. If a backend genuinely can't do FA, set
`AIMEE_LLM_SYNTH_KV_V=f16` (K8V8 does **not** help; any quantized V needs FA).
