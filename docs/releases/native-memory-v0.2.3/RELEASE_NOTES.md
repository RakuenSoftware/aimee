# Aimee native memory for vLLM: self-hosted binary preview

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

The model directory needs a matching `config.json`, tokenizer files (including
`chat_template.jinja` when supplied by the checkpoint), and
either safetensors or a single model GGUF. The launcher detects that GGUF
automatically. If there are multiple GGUF files, set `gguf_weights` to the
intended file. Qwen3.8 27B's tested hybrid profile uses vLLM's V1 runner;
leave `serve.v2_model_runner` false.

The exact GLIBC 2.34-compatible candidate passed an enrolled end-to-end check
with Gemma4 12B Q8 on an RTX 5080. It fetched two records over mTLS,
prepared one native bank, answered both facts with 34 prompt tokens, and
returned HTTP 503 after authorization was revoked. The preceding candidate
passed the same check with Gemma4 26B Q8 and Qwen3.8 27B Q8 on two RX 7900
XTXs, with 34 and 29 prompt tokens respectively; only the launcher build and
wheel integrity record changed. A separate test on the
preceding candidate recovered three selected facts with Qwen in one answer and in three
individual answers; its combined question used 27 prompt tokens. An earlier
Qwen candidate had a GGUF normalization conversion error in local preparation;
its failed results remain in the private validation record. The correction is
present in these signed bytes. These release-path tests used a local mTLS
primitive fixture with the standard thinclient profile; production Aimee
authorization deployment is a separate integration check. A fresh
`aimee:testing` server also accepted a genuinely enrolled thin client and
served the native primitive; it returned HTTP 401 for an invalid bearer. That
server held no writable memory records, so the real-server check did not test
selected-record answers. This plugin requires an Aimee application release
that includes `/v1/native/primitive`; public v0.4.6 did not at staging.

Initial native preparation can take minutes while the host reads and processes
model weights. The prepared bank is cached for later selections. This release
does not claim microsecond cold encoding or production throughput. The wheel
contains compiled private encoder modules and the portable Rust consumer; it
does not include model weights, vLLM, PyTorch or Aimee server binaries.
