# Native memory with a local vLLM model

The Aimee vLLM plugin lets a supported local model consume records selected by
your Aimee server as native attention memory. Aimee holds the source memory;
the machine running vLLM prepares and caches model-specific memory locally.
Your Aimee server does not download the model. The selected records do not
become chat messages or prompt tokens.

The first binary preview is staged for the separate
`native-memory-v0.2.3` GitHub Release. It requires a current Aimee server with
`POST /v1/native/primitive`; older servers do not provide the source primitive.
The plugin targets Linux x86-64, Python 3.12, glibc 2.34+, vLLM 0.30.x and a
BF16-capable CUDA or HIP runtime. Install vLLM for your GPU in a Python
environment before installing the plugin wheel into that same environment.

Enroll the existing Aimee thin client with an invitation from Settings →
Clients, as described in [Thin client](THIN_CLIENT.md). The plugin uses that
client's certificate, TLS trust and current bearer directly; it does not make
another credential copy.

```sh
aimee remote set https://your-aimee-endpoint INVITATION
aimee remote status
python -m pip install './aimee_vllm-0.2.3-1-cp312-cp312-linux_x86_64.whl[gguf]'
aimee-native connect-aimee
aimee-native serve --config ./aimee.json
```

Omit `[gguf]` if the model uses safetensors. A minimal `aimee.json` is:

```json
{
  "model": "/models/my-model",
  "serve": {"port": 8080, "tensor_parallel_size": 1}
}
```

The directory needs the matching `config.json`, tokenizer files and model
weights. A single model GGUF is found automatically; when there are multiple
GGUF files, set `gguf_weights` to the intended file. Initial preparation can
take minutes while the local host reads the weights. Reusable banks are cached
after preparation. The Qwen3.8 27B hybrid profile uses the V1 model runner;
leave `serve.v2_model_runner` false.

The staged wheel passed an enrolled end-to-end check on an RTX 5080 with a
Gemma4 12B Q8 GGUF-only model directory: it fetched two records over mTLS,
answered both facts with 34 prompt tokens, and refused a request after
authorization was revoked. Qwen3.8 27B Q8 prepared a GGUF-only memory bank;
its 28 GB GGUF was not run live on the 16 GB RTX 5080. These are tested model
profiles, not a claim that every model or GPU has been certified.
