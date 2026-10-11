# Native memory with a local vLLM model

The native-memory plugins let a supported local model consume Aimee-selected records through its
attention state. Aimee owns the source records and their authorization. The inference host owns
model weights, preparation and cached native state; selected memory stays outside the chat prompt.

![Aimee selects records while the enrolled inference host prepares model-specific attention memory](images/architecture/native-attention.svg)

The **1.0.1 application release preparation** includes five signed model bundles with plugin
version **0.3.3**, sharing `aimee-native-runtime==0.3.3`, build 5. Each bundle contains one model
adapter, its runtime and GGUF dependency, and verified Ed25519 signatures. Download the bundle
for your model from the [1.0.1 release files](releases/v1.0.1/README.md).
All five adapters passed [native-memory smokes](releases/native-memory-v0.3.3/qualification/README.md)
on one RX 7900 XTX with existing NAS GGUFs and zero CPU weight offload. Earlier 0.3.2 and 0.2.3
evidence belongs to its recorded bytes. The plugin is delivered with the regular application release.

## Choose the plugin for your checkpoint

| Model | Package and console command | Tested 0.3.3 checkpoint |
| --- | --- | --- |
| Gemma4 E2B | `aimee-gemma4-e2b` | UD-Q4_K_XL GGUF |
| Gemma4 E4B | `aimee-gemma4-e4b` | UD-Q4_K_XL GGUF |
| Gemma4 12B | `aimee-gemma4-12b` | UD-Q4_K_XL GGUF |
| Gemma4 26B A4B | `aimee-gemma4-26b` | Q3_K_M GGUF |
| Qwen3.8 27B | `aimee-qwen3-8-27b` | Q4_K_M GGUF |

These wheels require Linux x86-64, CPython 3.12, **glibc 2.39+ and vLLM 0.30.0**.
The final smokes used the `+rocm723` vendor build, ROCm 7.2, Torch 2.12.0 and Triton 3.7.1.
The tested mode is single-GPU, text-only, eager synchronous V1 serving with prefix caching disabled.
Earlier CUDA lifecycle matrices do not certify the final payload on every CUDA stack.

Each plugin carries its own Rust adapter and checked model binding. Common enrollment, selection,
supervision and vLLM integration live in the shared runtime. Co-installation preserves separate
namespaces; one serving process selects one adapter. Python/Cython remains the framework bridge.

## Install your signed model bundle and enroll

Choose one of the five [1.0.1 model bundles](releases/v1.0.1/README.md), extract it and follow that
page's pinned-key signature and checksum verification commands. Each bundle supplies exactly three
wheels: the selected adapter, shared runtime build 5 and GGUF loader. Install the verified files into
a Python 3.12 environment with the correct GPU-specific vLLM runtime already installed:

```sh
python -m pip install --no-index --no-deps /path/to/verified-bundle/*.whl
python -m pip check
```

Use a separate Python environment for each serving instance and install only its selected model's
bundle. These commands do not resolve missing dependencies from an index. Aimee binaries and model
weights are separate. Use your existing checkpoint storage.

Create an invitation in Settings → Clients and enroll the standard thin client as described in
[Thin clients](THIN_CLIENT.md). Connect the selected model command to that profile:

```sh
aimee remote set https://your-aimee-endpoint INVITATION
aimee remote status
aimee-gemma4-12b connect-aimee --aimee-home /absolute/path/to/enrolled-profile
# Supply VLLM_API_KEY in the process environment or a private service environment file.
aimee-gemma4-12b serve --config ./serving.json
```

Use the matching console command for another model. `--root /absolute/path` before a
subcommand selects a separate managed instance. The plugin retains the profile path and reads its
current bearer, certificate and TLS pin. A changed enrolled identity requires reconnecting and
restarting the instance. Serving requires a nonempty `VLLM_API_KEY` and binds to loopback.

## Bind preparation to the serving model

Your serving configuration identifies the local model directory, matching tokenizer and
`config.json`, checkpoint precision and host/device/disk memory budgets. Gemma GGUF configurations
bind `gguf_weights`, `gguf_weights_sha256` and `gguf_weight_profile`. Keep Qwen on V1 with
prefix caching disabled and provide its complete attention and recurrent prefix. A bank prepared
for another checkpoint, precision or position protocol is rejected.

The Qwen Q4 profile is bound to the tested checkpoint SHA-256
`31629f53165ab6a7dad8c9847dcfd1fdf55829dac1e6e748f4a68581b0033d34`.
Use the text-only causal configuration, without a vision configuration or projector. Its tested
native device preparation budget is 256 MiB; 128 MiB is insufficient for the recurrent state.

Cold or changed source is prepared through the resident model while admission is withdrawn.
Preparation loads no second model. Every selection rechecks current source authority, including
warm cache reuse. Gemma uses stock positive-position capture; older negative-position publications
must be re-encoded. The enrolled path currently selects personal memory. Shared organization
memory needs its separate authorization and selection contract.

The server needs `POST /v1/native/primitive` plus unique, bounded source export. Published Aimee
0.4.6 lacks that route. The pinned testing image in the
[release manifest](releases/native-memory-v0.3.3/manifest.json) provides it and passed the model
smokes. PR #3013 changed documentation and artifacts, so it required no new application image.
Application 1.0.1 publication follows the regular release process; the files in this PR are signed plugin bundles.

Storage and retrieval remain selected through the [memory backend contract](modules/memory.md#memory-backend-contract).
Changing native/Cognee retrieval and installing a model attention plugin are separate operations.
