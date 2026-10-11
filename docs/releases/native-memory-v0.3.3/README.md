# Five native-memory plugins at 0.3.3

The 0.3.3 candidate contains dedicated Gemma4 E2B, E4B, 12B, 26B A4B and Qwen3.8 27B
adapters, shared-runtime build 5 and the pinned GGUF loader. The original candidate wheels are included
for review. The same wheel bytes are now supplied with verified signatures in the
[five 1.0.1 model bundles](../v1.0.1/README.md). It supersedes the mixed 0.3.2 stages;
[their evidence](../native-memory-v0.3.2/README.md) remains historical.

[manifest.json](manifest.json) pins all seven wheel names, sizes, SHA-256 digests and proposed
public URLs. [SHA256SUMS](SHA256SUMS) also pins the manifest. Private source, model weights,
credentials and signing material are excluded from this public artifact stage.

## Final runtime passes all five NAS-backed model smokes

All five adapters passed seven native-memory checks each on one RX 7900 XTX, with no CPU weight
offload, against the pinned Aimee testing image. The checks cover denied unauthenticated inference,
authorized source storage, cold recall, warm recall and warm bank reuse. The selected canary stays
outside the chat prompt. [Qualification](qualification/README.md) records exact bytes and limits.
These short smokes do not establish exhaustive lifecycle or performance qualification for every
checkpoint, hardware stack or serving mode.

Shared-runtime build 5 fixes the text-only Qwen GGUF architecture mapping. It retains the
SHA-bound Q4 capture profile introduced in build 4. Qwen's tested device preparation budget is
256 MiB; 128 MiB correctly refused a recurrent state larger than that budget. Earlier CUDA and
extended small-model results belong to their recorded artifact versions.

## Delivery with application 1.0.1

The plugin payload is included in the regular application 1.0.1 release preparation as
[five model bundles](../v1.0.1/README.md), one file per model. Each bundle contains its adapter,
shared-runtime build 5 and GGUF loader, plus their existing Ed25519 signatures and signed metadata.
The installation guide provides pinned-key verification and offline installation commands.
No separate native-memory GitHub release is used.

The compatible application needs `POST /v1/native/primitive`, unique source versions, a 32-record
cap and refusal of conflicting source rows. The tested application digest remains recorded in the
manifest. The original candidate manifest and qualification records retain their historical bytes;
the signed delivery metadata is inside each 1.0.1 bundle.
