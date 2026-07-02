# aimee-qwen-delegate — CPU-only Qwen 3.6 27B coding delegate

A dedicated, single-model, OpenAI-compatible **coding delegate** for aimee: one
llama.cpp `llama-server` serving a baked **Qwen 3.6 27B** GGUF entirely on CPU, and
registered against aimee-server as a local delegate. It is independent of the
`aimee-llm` embed/rerank/synth stack and does not use the GPU.

## Model

| | |
|---|---|
| Repo / file | `unsloth/Qwen3.6-27B-GGUF` / `Qwen3.6-27B-UD-Q6_K_XL.gguf` |
| Quant | Unsloth Dynamic 2.0, **Q6_K_XL** (UD) |
| Size | ~23.9 GiB (25,636,485,344 bytes) |
| HF revision | `82d411acf4a06cfb8d9b073a5211bf410bfc29bf` |
| sha256 | `8746881d40f280b1b6b858c656a347c754ed3d9cc8d2e1ad46b3635b87f611f8` |

The GGUF is **baked into the image** and pinned by revision OID + sha256 (verified at
build time — a compromised/MITM'd upload fails the build, not production).

## Why 4×128K is affordable on CPU

Qwen 3.6 27B is a **dense** model but uses a **Gated-DeltaNet hybrid** attention stack:
64 layers = 16 blocks × (3 Gated-DeltaNet + 1 Gated-Attention). The linear DeltaNet
layers keep **no KV cache**, so only the **16 attention layers** cache KV. That makes a
large aggregate context practical in system RAM.

- Aggregate context defaults to **512K = 4 slots × 128K** (`--ctx-size 524288
  --parallel 4`). Each slot gets `CTX/SLOTS = 131072`.
- Native context is **262K**, so each 128K slot sits below native → **no RoPE/YaRN
  scaling**.
- KV defaults to **q8_0 + flash-attention** (`-fa on`), roughly halving KV RAM vs f16.

Runs on llama.cpp **`b9775`**, the same release already serving the MoE sibling
(Qwen 3.6 35B-A3B) in aimee — so the architecture is supported. CPU-only build
(`llama-…-bin-ubuntu-x64`, not the Vulkan build); `-ngl 0`.

## Tuning knobs (image ENV; override via plugin config, no rebuild)

| Env | Default | Meaning |
|---|---|---|
| `AIMEE_DELEGATE_CTX` | `524288` | aggregate context across all slots |
| `AIMEE_DELEGATE_SLOTS` | `4` | parallel slots (`--parallel`) |
| `AIMEE_DELEGATE_THREADS` | `16` | `--threads` (physical cores) |
| `AIMEE_DELEGATE_NGL` | `0` | GPU layers (0 = CPU-only) |
| `AIMEE_DELEGATE_FA` | `on` | flash-attention (required for quantized KV) |
| `AIMEE_DELEGATE_KV_K` / `_KV_V` | `q8_0` / `q8_0` | KV cache types |
| `AIMEE_DELEGATE_MODEL_ID` | `qwen3.6-27b` | served model alias |
| `AIMEE_DELEGATE_PORT` | `8744` | listen port |
| `AIMEE_LLM_AUTH_TOKEN` | *(unset)* | bearer for `/v1`; empty = auth-off |

**RAM fallback:** if flash-attention misbehaves on the CPU build, the server fails
loudly at load (quantized KV requires FA). Set `AIMEE_DELEGATE_FA=off` +
`AIMEE_DELEGATE_KV_K=f16` + `AIMEE_DELEGATE_KV_V=f16` and re-materialise — f16 KV is
guaranteed to run, at higher RAM cost.

## Deploy (SmoothNAS plugin, e.g. .254)

1. Pre-pull the image via the runtime docker socket (ONE detached pull — concurrent
   pulls of the same tag clobber under LXC2Docker/skopeo).
2. Install the plugin `deploy/smoothnas/aimee-qwen-delegate.plugin.yaml` via the tierd
   API (`POST /api/auth/login` → `POST /api/plugins/install` → `materialise` →
   `start`).
3. Register it as an aimee delegate (writes `agents.json` + `aimee.yaml` concurrency;
   the running server mtime-reloads both):

   ```
   aimee agent local qwen3.6-27b http://10.100.0.1:8744/v1 \
       --model qwen3.6-27b --provider openai --slots 4 --ctx 131072 \
       --roles code,reason,execute --cost-tier 0
   ```

## Verify

- `GET :8744/health` → 200 (allow minutes for the cold load of a ~24GB model on CPU).
- `POST :8744/v1/chat/completions` (model `qwen3.6-27b`) returns a completion; with
  `--jinja` the `<think>…</think>` reasoning block parses.
- `GET :8744/slots` shows **4** slots.
- `aimee agent probe qwen3.6-27b` → `model_available:true`, `detected_slots:4`,
  `execution_ok:true`.
