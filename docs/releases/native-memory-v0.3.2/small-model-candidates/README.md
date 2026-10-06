# E2B and E4B qualification candidates

Both 0.3.2 build-4 plugins passed CPU adapter checks and actual vLLM CUDA Q8_0
text-serving qualification. Each passed a 26-check short-prefix lifecycle and a
27-check multi-chunk lifecycle using its wheel downloaded from PR #3013. The
latter publishes a 240-token native bank across two GPU prefill chunks.

The checks include cold/warm recall, correction, retirement, streaming, concurrent
requests, real source outage/recovery, recipient isolation and server revocation.
See [qualification scope and results](VALIDATION.md) and the named JSON reports.
Both also passed 27/27 extended checks on 7900 XTX / ROCm with the corrected
shared-runtime build-3 candidate. The 12B, 26B and Qwen 27B native smoke tests
passed 18/18 each on their [named hardware profiles](../rocm-qualification/README.md).

Each plugin follows the existing dedicated Rust adapter and thin binding pattern,
with one `aimee-native-runtime==0.3.2` dependency. The original CUDA runs use
signed runtime build 2. Both models also repeated all 27 CUDA checks with
corrected runtime build 3, matching the ROCm runs. Model-specific bindings handle
shared K/V capture and the small models' per-layer GGUF weights. Build 4 corrects
the mapping and replicated-projection loading defects found during qualification.
Historical reports retain their original hashes; the current wheel bytes are
pinned by `small-wheel-audit-build4.json` and `pr3013-build4-download.json`.

The wheels remain unsigned candidates. The existing signed manifest and verifier
cover the original three-model stage. Final metadata, signing and public release
installation must cover these selected bytes before publication. Model weights,
credentials, private build inputs and signing keys are excluded from this PR.
