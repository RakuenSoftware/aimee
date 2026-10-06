# Current testing passes application and native-model validation

The current `:testing` application passed **2,314 of 2,314 fresh installed-runtime checks**
on 6 October 2026. All five final 0.3.3 native adapters also passed seven cold/warm smoke checks
each against this same application digest. No runtime blocker was found in the tested profiles.
The application is ready for main-merge review. Native artifact signing, publication and installation
from public downloads remain separate release gates.

## Exact application bytes

The registry's moving `:testing` and immutable `:testing-73cd98c` tags both resolve to:

```text
ghcr.io/rakuensoftware/aimee@sha256:e42752b9aafa9703ce9f11a703503a8bf0ea4fbe58dd6abbc8ccab5699904f0b
```

The installed tests used that digest in owned CT9210 on .253. Fresh fixture volumes, provider data,
caches and logs were stored on .254. Existing images were used with `--pull never`, and model-hub
access was disabled. No new model downloads were required.

Current testing source is `2a9fae736b806153d4399172b1d83e721cbb02f6`. Since the image publication,
changes consist of documentation, plugin artifacts and the application-source hash-lock refresh.
PR #3013 changed no application runtime code and did not require a new application image.

## Fresh installed-runtime checks

| Suite | Passed | Failed |
| --- | ---: | ---: |
| cognee | 119 | 0 |
| vault | 18 | 0 |
| T1 | 207 | 0 |
| T2 | 1260 | 0 |
| T3 | 710 | 0 |
| Total | 2,314 | 0 |

[Results](current-testing-release-2026-10-06/results.json) retain each assertion and suite identity.
T2 and T3 include receipt-health persistence and public-principal isolation. Those additional gates
add 22 checks to the earlier 2,292-check qualification. Counts include nested suite assertions and
their parent completion gates; they are not a count of distinct user actions.

Cognee uses the real 1.6.2 provider with a deterministic local CPU completion/embedding fixture.
The 119 checks exercise Aimee's canonical authority, personal/shared access, HTTP/CLI/MCP,
outages, credential rotation, correction, conditional deletion, capacity refusal, restart and managed
erasure while preserving another principal's records. This qualifies the Aimee integration;
it does not benchmark retrieval quality or cover every upstream Cognee feature.

## Final native plugins

[Model qualification](../releases/native-memory-v0.3.3/qualification/README.md) records all five
models and 35 passing smoke checks. Each used one RX 7900 XTX, existing GGUFs from .254 storage,
and zero CPU weight offload. E2B, E4B and 12B used UD-Q4_K_XL, 26B used Q3_K_M, and Qwen3.8
27B used Q4_K_M. All ran shared-runtime 0.3.3 build 5, SHA-256
`a6ddd22b908b8195e51e7a65953fd66f8987c9eb7944ed67bc5f6fcb14ed4511`.

The checks cover admission withdrawal, unauthenticated inference/source refusal, authorized canary
storage, actual cold and warm native recall, and warm bank reuse. Their scope is short single-GPU,
text-only serving. Recorded loader warnings, fallback behavior and Qwen's 256 MiB preparation
budget are described alongside the model evidence.

## CI and delivery gates

[Current-head CI](https://github.com/RakuenSoftware/aimee/actions/runs/37502336063) passed all
56 jobs. The [main-merge workflow](https://github.com/RakuenSoftware/aimee/actions/runs/37502337518)
passed its 21 preapproval jobs; three conditional jobs were skipped and the approval job awaits
`main-merge-approval`. No main merge or release approval was performed by this review.

[PR #3014](https://github.com/RakuenSoftware/aimee/pull/3014) contains the final five-model 0.3.3
wheel payload, manifest, checksum list, signature-enforcing verifier and corrected active documentation.
Merge that release preparation before promoting PR #3003. The seven-wheel payload is unsigned and
unpublished. It still requires nine controlled-key signatures, reviewed publication and independent
installation from verified public downloads against the published application.
