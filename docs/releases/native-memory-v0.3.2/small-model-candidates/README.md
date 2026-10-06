# E2B and E4B qualification candidates

Both 0.3.2 build-4 plugins passed CPU adapter checks and actual vLLM CUDA Q8_0
text-serving qualification. Each passed a 26-check short-prefix lifecycle and a
27-check multi-chunk lifecycle using its wheel downloaded from PR #3013. The
latter publishes a 240-token native bank across two GPU prefill chunks.

The checks include cold/warm recall, correction, retirement, streaming, concurrent
requests, real source outage/recovery, recipient isolation and server revocation.
See [qualification scope and results](VALIDATION.md) and the named JSON reports.
The adapters are ready for the larger-model smoke stage on this tested profile.

Each plugin follows the existing dedicated Rust adapter and thin binding pattern,
with the unchanged `aimee-native-runtime==0.3.2`. Model-specific bindings handle
shared K/V capture and the small models' per-layer GGUF weights. Build 4 corrects
the mapping and replicated-projection loading defects found during qualification.
Historical reports retain their original hashes; the current wheel bytes are
pinned by `small-wheel-audit-build4.json` and `pr3013-build4-download.json`.

The wheels remain unsigned candidates. The existing signed manifest and verifier
cover the original three-model stage. Final metadata, signing and public release
installation must cover these selected bytes before publication. Model weights,
credentials, private build inputs and signing keys are excluded from this PR.
