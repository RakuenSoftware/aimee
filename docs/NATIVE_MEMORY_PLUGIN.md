# Native memory with a local vLLM model

The Aimee vLLM plugin lets a supported local model consume records selected by
your Aimee server as native attention memory. Aimee holds the source memory;
the machine running vLLM prepares and caches model-specific memory locally.
Your Aimee server does not download the model. The selected records do not
become chat messages or prompt tokens.

![Aimee selects records while the enrolled inference host prepares model-specific attention memory](images/architecture/native-attention.svg)

The storage and retrieval backend remains selected through Aimee's
[generic memory contract](modules/memory.md#memory-backend-contract). This model plugin has a
separate lifecycle from that engine selection and from the application 1.0.0 release.

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

The staged wheel passed enrolled end-to-end checks with Gemma4 12B Q8 on an
RTX 5080, and Gemma4 26B Q8 and Qwen3.8 27B Q8 on two RX 7900 XTXs. Each
answered two selected records from one native bank and refused a request
after authorization was revoked. Prompt-token counts were 34, 34, and 29.
The wheel also runs in a GLIBC 2.34 container. A separate fresh Aimee
testing server accepted an ordinary enrolled thin client and rejected an
invalid bearer with HTTP 401; that server held no writable memory records.
These are tested profiles, not certification of every model or GPU. Aimee's
public v0.4.6 did not yet include the primitive route at staging.
