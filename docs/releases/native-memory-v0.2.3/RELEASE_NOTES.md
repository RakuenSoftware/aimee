# Aimee native memory for vLLM — self-hosted binary preview

This release lets a supported local vLLM model consume records selected by
your own Aimee instance as native attention memory. The records are not added
to the chat prompt or counted as prompt tokens. Aimee retains the source
memory; initial model-specific preparation and the cache stay on the machine
that runs vLLM. The Aimee server does not download your model.

The plugin uses the **existing Aimee thin-client enrollment**. In Aimee's
Settings → Clients, create an invitation, then enroll the thin client normally.
The plugin reads that enrolled profile directly, including certificate, TLS
pin and current bearer. There is no second plugin enrollment or credential
copy. When Aimee withdraws access, native serving refuses the request.

The binary wheel targets Linux x86-64, Python 3.12, glibc 2.34+, vLLM 0.30.x
and a BF16-capable CUDA or HIP runtime. Install vLLM for your GPU first, then
install the wheel in **that same Python environment**. A GGUF model needs the
`[gguf]` extra; safetensors does not.

```sh
python -m pip install './aimee_vllm-0.2.3-1-cp312-cp312-linux_x86_64.whl[gguf]'
aimee remote set https://your-aimee-endpoint INVITATION
aimee-native connect-aimee
aimee-native serve --config ./aimee.json
```

`aimee.json` can be as small as:

```json
{
  "model": "/models/my-model",
  "serve": {"port": 8080, "tensor_parallel_size": 1}
}
```

The model directory needs a matching `config.json`, tokenizer files, and
either safetensors or a single model GGUF. The launcher detects that GGUF
automatically. If there are multiple GGUF files, set `gguf_weights` to the
intended file. Qwen3.8 27B's tested hybrid profile uses vLLM's V1 runner;
leave `serve.v2_model_runner` false.

The exact candidate wheel passed an enrolled end-to-end check on an RTX 5080
with Gemma4 12B Q8 in a GGUF-only directory: it fetched two records over mTLS,
prepared a local bank, answered both facts with 34 prompt tokens, and returned
HTTP 503 after authorization was revoked. Qwen3.8 27B Q8 also prepared a
GGUF-only two-record bank; that 28 GB checkpoint has not yet had a live
GGUF-only consumer check in this candidate because the 5080 cannot hold it.
Earlier vLLM consumer checks cover Qwen3.8 27B and Gemma4 12B/26B on RX 7900
XTX GPUs. This is a model-family preview, not certification of every model.

Initial native preparation can take minutes while the host reads and processes
model weights. The prepared bank is cached for later selections. This release
does not claim microsecond cold encoding or production throughput. The wheel
contains compiled private encoder modules and the portable Rust consumer; it
does not include model weights, vLLM, PyTorch or Aimee server binaries.
