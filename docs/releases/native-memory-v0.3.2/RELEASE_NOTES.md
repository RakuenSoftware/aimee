# Native memory 0.3.2: three model plugins on one runtime

Install the dedicated Gemma4 12B, Gemma4 26B A4B or Qwen3.8 27B plugin to consume Aimee-selected
personal memory through model attention. Each plugin pins `aimee-native-runtime==0.3.2` and carries
its own Rust adapter. Aimee owns canonical records and authorization; the inference host prepares
and caches the model state through its resident model.

The build-2 wheels target Linux x86-64, CPython 3.12, glibc 2.39+ and exactly vLLM 0.30.0.
Tested execution uses single-GPU CUDA, text-only eager synchronous V1 and no prefix caching:
Gemma 12B Q4_K_M, Gemma 26B A4B Q3_K_M and Qwen 27B FP8 with CPU matrix offload. Coverage of
older Q8/HIP artifacts does not transfer to this release.

Download all five wheels and the signed metadata, verify them with the repository's release
verifier, then install from the download directory:

```sh
python -m pip install --find-links . 'aimee-gemma4-12b[gguf]==0.3.2'
# Alternatives: 'aimee-gemma4-26b[gguf]==0.3.2' or 'aimee-qwen3-8-27b==0.3.2'
```

Use the [native-memory guide](https://github.com/RakuenSoftware/aimee/blob/testing/docs/NATIVE_MEMORY_PLUGIN.md) for the existing thin-client enrollment,
model command, API key and checkpoint configuration. The server needs `/v1/native/primitive` with
unique, bounded source export; application 0.4.6 is incompatible. The
[preparation record](https://github.com/RakuenSoftware/aimee/blob/testing/docs/releases/native-memory-v0.3.2/README.md) identifies the prerequisite and remaining delivery checks.

These notes prepare a release. The local candidate is signed, verified and unpublished; public download
and clean installation remain pending. Remove this pending-status paragraph only after those
checks are recorded against the published assets and compatible application image.


Gemma4 E2B and E4B build-4 plugins are additional qualified candidates on the same
runtime. They passed CUDA Q8_0 native lifecycle tests, including multi-chunk
capture, streaming, concurrency, source outage and revocation. Their wheels and
[qualification evidence](https://github.com/RakuenSoftware/aimee/blob/testing/docs/releases/native-memory-v0.3.2/small-model-candidates/VALIDATION.md)
are in the preparation PR. They remain unsigned and outside the original five-wheel
manifest; include them in release notes as delivered packages only after the final
signed asset set and verifier cover them.
