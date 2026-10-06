# Five native-memory plugins at 0.3.3

The 0.3.3 candidate contains dedicated Gemma4 E2B, E4B, 12B, 26B A4B and Qwen3.8 27B
adapters, shared-runtime build 5 and the pinned GGUF loader. The generated wheels are included
for review. **This candidate is unsigned and unpublished.** It supersedes the mixed 0.3.2 stages;
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

## Publish exact reviewed bytes

The compatible application needs `POST /v1/native/primitive`, unique source versions, a 32-record
cap and refusal of conflicting source rows. The tested application digest is in the manifest.
Application 1.0.0 publication and plugin publication have separate approval gates.

1. Sign all seven wheels, `manifest.json` and `SHA256SUMS` through the controlled Ed25519
   signing pipeline using the reviewed key. Do not replace the pinned trust key with a downloaded key.
2. Run the verifier below. All nine detached signatures are required. `--hashes-only` verifies
   candidate identities and cannot authorize publication.
3. Run [create_draft_release.sh](create_draft_release.sh) with the signed asset directory and exact
   reviewed application commit. It creates a draft prerelease with Latest disabled and 18 assets.
4. After publication, download all assets, verify them and test independent plugin installs,
   dependency consistency, enrollment and native recall against the published application.

```sh
python3 docs/releases/native-memory-v0.3.3/verify_assets.py /path/to/release-assets
```

[Release notes](RELEASE_NOTES.md) and the [plugin guide](../../NATIVE_MEMORY_PLUGIN.md)
describe installation and supported execution. Signing, publication and public-download installation
remain uncompleted gates; local smoke results do not substitute for them.
