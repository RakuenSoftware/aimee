# Tuning the aimee-delegate tiers

The `aimee-delegate` container runs one synth/delegate model (OpenAI `/v1`) on the
vendor-agnostic **Vulkan** llama.cpp build, split out of the unified `aimee-llm`
(embed+rerank+gateway) per the 2026-07-01 design roundtable. Two published tiers:

| Image | Model | Card | Profile (baked defaults) |
|---|---|---|---|
| `aimee-delegate:small` | Gemma 4 12B `qat-UD-Q4_K_XL` (6.72 GB, dense) | 16 GB | 96K ctx, 1 slot, K8V4 |
| `aimee-delegate:mid` | Qwen 3.6 35B-A3B `UD-Q4_K_XL` (22.4 GB, MoE) | 24–32 GB | 256K (2×128K), K8V4, static expert-offload |

Both GGUFs are pinned by **HF revision + sha256** and verified at build time; the
recorded hash is surfaced as an image `LABEL` (`org.aimee.delegate.sha256`) and echoed
in the container log for audit.

## The one knob you tune per card: `AIMEE_DELEGATE_N_CPU_MOE` (mid tier only)

Expert offload is **static by design** — there is no runtime VRAM auto-sizer. `N` =
how many of the 40 MoE layers keep their experts in **system RAM** instead of VRAM.
Higher `N` → less VRAM, more RAM traffic, slower. Value is clamped to `[0, 40]`.

Rules of thumb (Qwen mid tier, `UD-Q4_K_XL` ≈ 22 GB weights):

| Situation | Suggested `N_CPU_MOE` | Notes |
|---|---|---|
| 24 GB card **shared** with `aimee-llm` embed+rerank (~7 GB) | ~24 (default) | leaves room for retrieval stack + KV |
| 24 GB card **dedicated** to the delegate | ~16 | no retrieval reservation |
| 32 GB card shared | ~8 | most experts resident |
| 32 GB card dedicated | 0 | all experts on GPU, fastest |

**Calibrate once:** start with the table value, watch the container log. On success
you'll see `llama-server ready`. If you see `CRITICAL: allocation failure ... experts
likely CPU-fell-back`, raise `N` by 4 and redeploy. The healthcheck reports
**unhealthy** while a startup verification failure is latched, so a mis-sized `N`
surfaces as a failing container rather than silent slow throughput.

Free system RAM needed ≈ `N/40 × 22 GB` for the offloaded experts (e.g. `N=24` →
~13 GB). Make sure the host has it free alongside other LXC/containers.

**Measured baseline (7900 XTX, RADV, b9775).** Smoke-tested standalone on `.254` with
`N_CPU_MOE=40` (all experts on CPU — the *worst case*) + 16K ctx: ~**12 tok/s** generation,
~25 tok/s prompt, +2.6 GB GPU footprint. That's the floor; the shipped `N_CPU_MOE=24`
keeps ~16 expert layers resident and is meaningfully faster. Use this as the low-water
mark when sizing.

## Flash-attention / KV cache: `AIMEE_DELEGATE_KV_V`

K8V4 (`KV_K=q8_0`, `KV_V=q4_0`) is the default and is **verified working on RADV/gfx1100**
(7900 XTX, llama.cpp b9775) — the delegate loads and serves with it.

FA is enforced **structurally, not by log-scraping**: llama.cpp refuses to create a
context with a quantized V-cache unless flash-attention is active (`-fa off` +
`--cache-type-v q4_0` dies with *"V cache quantization requires flash_attn"*). The
entrypoint always passes `-fa on`, so **a healthy container already proves FA engaged** —
if a backend can't do FA, `llama-server` never starts and the container never goes
healthy (fail-loud). There is nothing to grep.

If you ever run on a backend that genuinely can't do FA, note that **K8V8 (`q8_0` V) does
NOT help — *any* quantized V-cache requires FA.** The only FA-free fallback is an
**unquantized V-cache**: set `AIMEE_DELEGATE_KV_V=f16` (larger KV, no FA dependency).

## Wiring into aimee

Compose (`aimee.delegate.yaml`) points `aimee-llm`'s gateway synth upstream at the
delegate and disables the kb's local synth:

```
AIMEE_LLM_SYNTH_LOCAL=0
AIMEE_LLM_SYNTH_URL=http://aimee-delegate:8083
```

The kb curator (`LLM_ENDPOINT=http://aimee-llm:8742/v1`) then reaches the delegate
through the gateway automatically. To also expose it to the server's delegate/round-
table router, register it once:

```
./aimee agent local delegate-mid http://aimee-delegate:8083/v1 \
    --model aimee-synth --roles code,reason,execute --cost-tier 0
```

## Selecting the small tier

```
AIMEE_DELEGATE_IMAGE=ghcr.io/rakuensoftware/aimee-delegate:small \
docker compose -f deploy/compose/aimee.yaml -f deploy/compose/aimee.gpu.yaml \
               -f deploy/compose/aimee.delegate.yaml up -d
```

The small image bakes dense/96K/1-slot defaults; drop the MoE env vars (they're
ignored when `AIMEE_DELEGATE_MOE=0`). The 16 GB tier is **headless-only** — it leaves
<1 GB free, so don't run it on a card also driving a display.
