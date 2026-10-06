# ROCm release qualification

Date: 2026-10-06. Host: `.253`, two RX 7900 XTX cards (`gfx1100`).

E2B passed the 18-check native-memory lifecycle on one card. E4B's extended
27-check multi-chunk matrix and the larger-model smoke tests are in progress.
This is not completed ROCm release qualification.

The first run exposed a shared-runtime version-check defect: the release rejected
`vllm==0.30.0+rocm723` although its public version is the pinned `0.30.0`.
The corrected check compares public versions. Eight installed compiled-module
checks accept 0.30.0 with vendor suffixes and reject other releases, prereleases
and postreleases. The rebuilt wheel's RECORD hashes and installed executable
permissions passed verification. Only the compiled compatibility module and
wheel build/RECORD metadata changed; model adapters and other runtime members
retain their original bytes.

The unsigned replacement runtime candidate is
`aimee_native_runtime-0.3.2-3-cp312-cp312-linux_x86_64.whl`, SHA-256
`1684ee2e5aa40ca9c0ed2818658de445bd112f440871372789e80709f9757bd2`.
It requires release signing and manifest inclusion. The existing signed build-2
runtime and its historical CUDA reports remain unchanged. CUDA validation of
this replacement runtime is still required.

The tested stack is CPython 3.12, vLLM `0.30.0+rocm723`, Torch
`2.12.0+git6bbd260`, HIP `7.2.53211` and Triton `3.7.1`. Each plugin has its
own environment, consuming the existing pinned ROCm packages read-only.
This establishes hardware/runtime behavior, not a clean-machine installation.
Small models use the PR build-4 wheel bytes, Q8_0, BF16, text-only synchronous
V1 eager execution, prefix caching disabled, a 2,048-token context and 128-token
prefill chunks. A standard enrolled mTLS client reads from the owned CT9210
Aimee fixture through loopback-only forwards.

`runtime-profile.json`, `runtime-build3.json`, `installed-runtime-contract.json`
and `gemma4-e2b-rocm-lifecycle.json` record the current evidence. Model weights,
private implementation source and authority credentials are excluded.
