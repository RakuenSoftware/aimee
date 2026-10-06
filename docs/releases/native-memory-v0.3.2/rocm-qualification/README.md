# ROCm release qualification

Date: 2026-10-06. Host: `.253`, two RX 7900 XTX cards (`gfx1100`).

E2B and E4B each passed all 27 checks in the extended multi-chunk native-memory
matrix, including source outage/recovery, streaming, concurrent requests,
frozen-recipient rejection and real server-side revocation. Both publish a
240-token native bank spanning two 128-token GPU prefill chunks. E2B also passed
a separate 18-check short-prefix lifecycle. All three larger-model smoke tests
passed 18/18 native lifecycle checks:

| Model | Tested profile | Result |
|---|---|---|
| Gemma 4 12B | One 7900 XTX, Q8_0/BF16 | 18/18 |
| Gemma 4 26B | One 7900 XTX, Q3_K_M/BF16 | 18/18 |
| Qwen 3.8 27B | RTX 5080, FP8/BF16, 18 GiB CPU offload | 18/18 |

Loaded-model memory was 12.31 GiB, 12.78 GiB and 9.49 GiB respectively.
The enrolled native launcher certifies a single-GPU profile; the initial TP2
attempt was rejected. Two earlier 12B fixtures omitted the required checksum or
quantization profile and failed closed. The corrected configuration binds the
verified checksum and profile; no admission guard was relaxed.

Qwen's current native capture requires FP8. Both vLLM kernel eligibility checks
and a real `torch._scaled_mm` probe rejected FP8 on `gfx1100` in the installed
stack. Its passing CUDA smoke does not qualify Qwen native capture on 7900 XTX.
The older GGUF/TP2 paths are separate historical qualifications.

Loaded-model memory was 4.81 GiB for E2B and 7.68 GiB for E4B. These values
exclude other allocations and are not peak-memory measurements.

The first run exposed a shared-runtime version-check defect: the release rejected
`vllm==0.30.0+rocm723` although its public version is the pinned `0.30.0`.
The corrected check compares public versions. Eight installed compiled-module
checks accept 0.30.0 with vendor suffixes and reject other releases, prereleases
and postreleases. The rebuilt wheel's RECORD hashes and installed executable
permissions passed verification. Only the compiled compatibility module and
RECORD contents changed; the filename carries the new build tag, and model adapters and other runtime members
retain their original bytes.

The unsigned replacement runtime candidate is
`aimee_native_runtime-0.3.2-3-cp312-cp312-linux_x86_64.whl`, SHA-256
`1684ee2e5aa40ca9c0ed2818658de445bd112f440871372789e80709f9757bd2`.
It requires release signing and manifest inclusion. The existing signed build-2
runtime and its historical CUDA reports remain unchanged. Qwen passed CUDA native lifecycle validation with this replacement runtime.
The small-model CUDA matrices are being repeated with these exact runtime bytes.

The tested stack is CPython 3.12, vLLM `0.30.0+rocm723`, Torch
`2.12.0+git6bbd260`, HIP `7.2.53211` and Triton `3.7.1`. Each plugin has its
own environment, consuming the existing pinned ROCm packages read-only.
This establishes hardware/runtime behavior, not a clean-machine installation.
Small models use the PR build-4 wheel bytes, Q8_0, BF16, text-only synchronous
V1 eager execution, prefix caching disabled, a 2,048-token context and 128-token
prefill chunks. A standard enrolled mTLS client reads from the owned CT9210
Aimee fixture through loopback-only forwards.

`runtime-profile.json`, `runtime-build3.json`, `installed-runtime-contract.json`
`runtime-member-audit.json`, `gemma4-e2b-rocm-lifecycle.json` and
`gemma4-e2b-rocm-multichunk.json` and `gemma4-e4b-rocm-multichunk.json` record the current evidence. Model weights,
private implementation source and authority credentials are excluded.
