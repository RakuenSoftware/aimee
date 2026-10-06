# E2B and E4B native adapter qualification

Date: 2026-10-06. Host: `.253`. Current artifacts: **0.3.2 build 4**.

Both adapters passed qualification on the tested CUDA Q8_0 text-serving profile
and are ready for the larger-model smoke stage. Their wheels are in PR #3013.
Additional [ROCm qualification](../rocm-qualification/README.md) found and corrected
a shared-runtime vendor-version check. Both models passed 27/27 extended ROCm
checks with its replacement build-3 runtime. The CUDA results below use the original build-2 runtime.
They remain unsigned candidates: release signing, inclusion in the final manifest
and installation from a published release are still required.

## Results

Counts below describe separate gates; they are not summed into one score.

| Gate | E2B | E4B |
|---|---|---|
| CPU native-memory lifecycle | 42/42 | 42/42 |
| Installed Rust admission checks | 45/45 | 45/45 |
| Actual vLLM CUDA lifecycle, short prefix | 26/26 | 26/26 |
| Actual vLLM CUDA lifecycle, multi-chunk prefix | 27/27 | 27/27 |
| GGUF tensor destination and shape audit | 600/600 | 719/719 |
| Real GGUF replicated projection loads | 70/70 | 84/84 |
| Projection conversion rejection/preservation checks | 6/6 | 6/6 |
| Shared-K/V capture checks in each clean environment | 5/5 | 5/5 |
| Independent installation, dependencies, console and imports | Passed | Passed |
| Final wheel audit and public PR download hashes | Passed | Passed |

Six Rust adapter tests, four existing layout/finalization regression tests,
20 installed identity/checkpoint contract checks and the original ten capture
binding checks also passed. Their reports retain their original artifact scope.

Each actual CUDA matrix checks cold and warm recall of a random six-digit canary,
bank reuse, conditional correction and a changed bank, retirement and abstention,
restoration, malformed authenticated requests, unsupported inference endpoints,
caller publication override rejection, complete SSE streaming, subsequent recall,
two concurrent requests, real source outage and recovery, frozen-recipient
rejection, original-recipient recovery and real server revocation. The canary is
absent from the chat prompt. Aimee supplies source records through a standard
enrolled mTLS client with a pinned server certificate.

The long-prefix runs use wheel bytes downloaded directly from PR #3013. Both
publish a 240-token native bank, spanning two 128-token GPU prefill chunks, and
repeat the complete lifecycle. This exercises the new shared-K/V source binding
across real chunk boundaries. All fixtures were cleaned up, the owned source
container recovered, and both owned model services stopped. The larger-model
smoke tests have not started.

## Qualified profile

Linux x86-64, CPython 3.12, vLLM 0.30.0, Torch 2.13.0, Triton 3.7.1 and an RTX
5080. Serving uses BF16 attention/cache, Q8_0 GGUF weights, one GPU, text-only eager
synchronous V1 execution, prefix caching disabled, a 2,048-token context,
128-token prefill chunks, one scheduled sequence and a 1,024-block KV-cache cap.
Native bank budgets are 1 GiB host and 128 MiB device.

Reported loaded-model memory is 4.89 GiB for E2B and 7.76 GiB for E4B. These
figures exclude other GPU allocations and are not peak-memory measurements.
ROCm/HIP, arbitrary quantizations, multimodal requests and a vLLM CPU serving
port are outside this qualification. The CPU lifecycle uses the established
Transformers attention harness with installed Rust publication/admission and
position mapping; it does not qualify a CPU port of the vLLM backend.

The tested application is
`ghcr.io/rakuensoftware/aimee@sha256:e42752b9aafa9703ce9f11a703503a8bf0ea4fbe58dd6abbc8ccab5699904f0b`,
from source `73cd98ce5b350323dd8956bb750d45a500ead3db`.
The `:testing` registry manifest was rechecked after the final runs and still
resolved to this digest.

## Model and artifact identity

HF geometry/tokenizer inputs are pinned to `google/gemma-4-E2B-it` revision
`3e22461f65e89153144f8adb70e3b8c2cc9845a7` and `google/gemma-4-E4B-it` revision
`ee0ef6023621cff504d758262d4e04895a5af4a2`. E4B's CPU safetensors file matched
SHA-256 `cfbd3d2f1cd71bd471c37fe2bf8546d5028d41e5736f64e1ca6c6b8893125503`.
E2B used the existing verified CPU checkpoint on the research host.

| Q8_0 checkpoint | Pinned revision | SHA-256 |
|---|---|---|
| `ggml-org/gemma-4-E2B-it-GGUF` | `b4243c156154b6dca9324415f8c7ccc098b4aed1` | `996d08777aadc6bfd3c7375ef70ba25a0f55240075860754fdb18d6d860aa63a` |
| `ggml-org/gemma-4-E4B-it-GGUF` | `b8093469224f83f5c38f691eb906c380e9e63114` | `34be82b17b4942d389b9b527170c4b058027abdd32531fda063d3d97dd8ce80a` |

| Current wheel | SHA-256 |
|---|---|
| `aimee_gemma4_e2b-0.3.2-4-cp312-cp312-linux_x86_64.whl` | `3f9370f8a0c00b0f3b0c64b393fa896acd3c38fd0670152872d966255514731e` |
| `aimee_gemma4_e4b-0.3.2-4-cp312-cp312-linux_x86_64.whl` | `4f0f7292559e21138e120c4b435dac632a0e8dd19d511193008d9591640d58d0` |

Both Rust libraries are byte-identical to those used in CPU lifecycle validation.
The shared `aimee-native-runtime==0.3.2` bytes and the pinned GGUF dependency remain
unchanged. Each model retains its own Rust adapter, binding, namespace and command.
Rust owns geometry, shared-K/V source mapping, publication admission and positions.
Model-specific framework bindings handle shared K/V capture and GGUF tensor loading.

## Defects found and corrected

Build 2 omitted small-model per-layer embedding/projection weights from GGUF
mapping. Build 3 maps every loaded tensor to the pinned HF destination and shape,
and rejects unknown root tensors, duplicate destinations and missing per-layer
embedding weights. Only the derived rotary-frequency tensor is omitted.

Actual build-3 loading found that vLLM's `ReplicatedLinear` loader cannot receive
packed GGUF parameters. Build 4 materializes the small per-layer gate/projection
matrices as BF16 before loading; embeddings and other weights retain the existing
GGUF path. Every real converted matrix passed the actual loading function on CPU,
then both complete models passed CUDA inference.

A fixture cap of 128 KV blocks was insufficient for a 2,048-token context despite
available GPU memory. Increasing it to 1,024 blocks resolved startup. The outage
harness also needed to enter the host network namespace for Proxmox control.
These fixture changes did not change the wheels.

Earlier CPU attempts exposed a missing `accelerate` harness dependency, an
assertion incorrectly treating the allowed absence answer `unknown` as leakage,
and cleanup rejecting HTTP 404 for an already retired record. Standard enrollment
needed its CLI retry. The projection-loading test initially constructed a layer
without a vLLM parallel group; it was corrected to call the real loading function
with a CPU parameter. Passing runs followed each correction.

One E4B GGUF download was truncated and rejected by its checksum before loading.
It was quarantined; a fresh download passed verification. A brief SSH interruption
caused one E2B startup to be stopped during tokenizer loading; its cause is
unconfirmed. Another startup correctly refused an occupied GPU and left the
unrelated research job running.

Historical build-2/build-3 reports and hashes remain unchanged. Failed attempts
remain archived on the private research host. Named-verdict reports are alongside
this file. Model weights, credentials, private build inputs and signing keys
remain outside the public application repository.
