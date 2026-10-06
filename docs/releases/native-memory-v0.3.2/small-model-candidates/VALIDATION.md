# E2B and E4B native adapter validation

Date: 2026-10-06. Host: `.253`.

Two additional model plugins built and passed CPU native-memory validation:
`aimee-gemma4-e2b` and `aimee-gemma4-e4b`. Each model passed 42 lifecycle checks,
with six real inference requests and five native-bank preparations. The stock
model parameters remained on CPU and unchanged. Both fixtures were cleaned up.

The plugins follow the existing aimee-qwen pattern: one dedicated Rust adapter
and library, a checked binding contract, separate command and module namespace,
and the unchanged `aimee-native-runtime==0.3.2` dependency. Rust owns model
geometry, shared K/V source mapping, publication admission and live positions.
The model-specific capture binding handles upstream shared layers without
copying the common serving runtime.

| Gate | Result |
|---|---|
| E2B CPU lifecycle | 42/42 |
| E4B CPU lifecycle | 42/42 |
| Installed plugin identity and checkpoint contracts | 20/20 |
| Shared K/V capture binding | 10/10 |
| Rust adapter tests, including both new model geometries | 6/6 |
| Existing layout and finalization regression tests | 4/4 |
| Final wheel RECORD, namespace, symbols, shared dependency and metadata audit | Passed |

Lifecycle checks include cold and warm recall of a random six-digit canary,
warm bank reuse, conditional correction and a changed bank, retirement and
abstention, restored memory, changed-recipient rejection, and real server
revocation before another decode. Aimee supplied the current source through a
standard enrolled mTLS client with a pinned server certificate. The canary was
absent from the main conversation. The main conversation cache remained unchanged
after each private decode. Native banks were published and admitted through the
installed Rust adapter, then read by the established Transformers CPU attention
harness. Recipient-change rejection in this run is enforced by that harness;
it is not a separate qualification of the vLLM serving broker.

The application image was
`ghcr.io/rakuensoftware/aimee@sha256:e42752b9aafa9703ce9f11a703503a8bf0ea4fbe58dd6abbc8ccab5699904f0b`,
from source `73cd98ce5b350323dd8956bb750d45a500ead3db`. The current `:testing`
manifest was checked after execution and still resolved to that digest.

E2B used the existing verified CPU checkpoint at revision
`3e22461f65e89153144f8adb70e3b8c2cc9845a7` of `google/gemma-4-E2B-it`.
E4B used revision `ee0ef6023621cff504d758262d4e04895a5af4a2` of
`google/gemma-4-E4B-it`; its 15,992,595,884-byte safetensors file matched the
upstream SHA-256 `cfbd3d2f1cd71bd471c37fe2bf8546d5028d41e5736f64e1ca6c6b8893125503`.

The first E2B attempt lacked the harness dependency `accelerate`. A later
attempt found an assertion bug: the absence response `unknown` was already
allowed in the prompt and must not be treated as leaked memory. Cleanup also
needed to accept HTTP 404 for an already retired fixture. These harness defects
were corrected before the full passing runs. Failed attempts remain archived.

This validates CPU model and adapter behavior. The vLLM serving backend, GPU
execution, and public signed release installation remain unqualified by these
runs. The final wheels are unsigned local validation candidates. Their Rust
libraries are byte-identical to the libraries used during inference; the final
shared-K/V binding passed its structural tests. Finalization added dependency
metadata and notices while preserving executable members.

| Plugin | Earlier build-2 wheel SHA-256 |
|---|---|
| gemma4-e2b | `aab9e90748151e6d700aaad1d60585bef055a35fab5b1c2b73098f0abdb4f4c0` |
| gemma4-e4b | `be890245b08e05cd2878dce3686e095699d5064a3f31d4f895745e3c9eb461d2` |

Raw named-verdict reports are alongside this file. Model weights, credentials,
and private source remain outside the public application repository.

## Release qualification in progress

The two wheel files were downloaded independently from PR #3013 and matched
both SHA-256 values above. Each passed a fresh isolated environment installation,
dependency checks, console startup and serving-module imports. The shared K/V
capture tests also passed in each separate clean environment.

Each installed Rust adapter passed 45 additional admission checks: valid position
mapping and complete segments; rejection of changed geometry, rotary parameters,
incomplete or duplicate segments, invalid position ranges, memory lengths outside
the admitted bound and integer overflow. These tests do not run model inference.

Actual vLLM CUDA lifecycle validation started on the RTX 5080 using the E2B Q8_0
checkpoint. Standard client enrollment succeeded after adding the test harness's
CLI retry. The serving process started, but no completed inference or lifecycle
verdict was observed. SSH briefly stopped responding while ICMP remained
reachable. After SSH recovered, the owned model unit was confirmed stopped
during tokenizer loading, before inference. Memory pressure was low after recovery
and I/O pressure was elevated. The cause of the SSH interruption is unconfirmed;
the attempt remains archived.

A subsequent run refused to start because the GPU was occupied by the separate
`jmlr-gemma-prefix-integration-recovery-20261006-r4` research service. That job was
left running. Actual native vLLM inference and its lifecycle failure checks remain
pending. Both candidates remain unsigned and are not release-qualified. The
12B, 26B and Qwen 3.8 27B smoke tests have not started.

Both Q8_0 GGUF checkpoints matched their pinned upstream hashes:

| Model | Repository revision | SHA-256 |
|---|---|---|
| E2B | `ggml-org/gemma-4-E2B-it-GGUF@b4243c156154b6dca9324415f8c7ccc098b4aed1` | `996d08777aadc6bfd3c7375ef70ba25a0f55240075860754fdb18d6d860aa63a` |
| E4B | `ggml-org/gemma-4-E4B-it-GGUF@b8093469224f83f5c38f691eb906c380e9e63114` | `34be82b17b4942d389b9b527170c4b058027abdd32531fda063d3d97dd8ce80a` |

The first E4B download was truncated and rejected by its checksum check. Those
bytes were quarantined. A fresh download passed verification before any model
load. No rejected checkpoint was loaded.

## Build 3: per-layer GGUF weights

The checkpoint audit found that the upstream Gemma GGUF mapper omitted the
small models' per-layer token embeddings, model projection, projection norm,
and three per-layer input tensors. Build 3 adds a dedicated model-specific
loader mapping these tensors. The common serving runtime is unchanged. Unknown
root tensors, duplicate destinations and missing per-layer embedding weights
are rejected. Derived rotary frequencies are omitted, as in the existing loader.

All 600 E2B and 719 E4B loaded GGUF tensor destinations and shapes matched the
pinned HF safetensors metadata. Both rebuilt wheels passed the wheel audit;
their Rust libraries remain byte-identical to those used in the CPU lifecycle
runs. The build-2 wheels have been replaced in this PR; historical build-2 reports
retain their original hashes. Build 3 is installed in the separate clean
environments and actual E2B vLLM serving qualification has resumed.

| Plugin | Earlier build-3 wheel SHA-256 |
|---|---|
| gemma4-e2b | `fce3936772353eed603b5c4e34bd953cd9da2e813a71e65fc709c96b04699986` |
| gemma4-e4b | `5ef67b2d20ffcd3dadb711e2aa227c5ddca17996702c44b0fbde54722e955b07` |

The mapping audit checks metadata; it does not establish successful model loading,
quantized kernel execution or native recall. Those remain required release gates.

## Build 4: replicated per-layer projections

Actual E2B vLLM loading reached the mapped per-layer gate and failed: vLLM's
`ReplicatedLinear` loader cannot receive packed GGUF lazy parameters. Build 4
materializes only the per-layer gate and projection matrices as BF16 before
loading; embeddings and other weights retain the existing GGUF path. This is
implemented in the two model plugins, with no shared runtime change.

All 70 E2B and 84 E4B real quantized projection matrices passed conversion,
finite-value and shape checks, then loaded through the actual vLLM
`ReplicatedLinear.weight_loader` into CPU parameters with exact equality.
This checks the loading function without a GPU model execution. The first test
attempt tried constructing a full linear layer without initializing vLLM's
parallel process group; the test was corrected to call its real loading function
with a parameter directly.

Both build-4 wheels passed the wheel audit and are installed in the independent
environments. Their Rust libraries remain byte-identical to the CPU-qualified
libraries. The build-3 files are replaced; their reports retain original hashes.
Actual E2B GPU serving is being retried. Neither plugin is release-qualified yet.

| Plugin | Current build-4 wheel SHA-256 |
|---|---|
| gemma4-e2b | `3f9370f8a0c00b0f3b0c64b393fa896acd3c38fd0670152872d966255514731e` |
| gemma4-e4b | `4f0f7292559e21138e120c4b435dac632a0e8dd19d511193008d9591640d58d0` |
