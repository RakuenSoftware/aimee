# Five native-memory plugins at 0.3.3

The [0.3.3 release](https://github.com/RakuenSoftware/aimee/releases/tag/native-memory-v0.3.3) contains dedicated Gemma4 E2B, E4B, 12B, 26B A4B and Qwen3.8 27B
adapters, shared-runtime build 5 and the pinned GGUF loader. The reviewed wheels are published
with detached Ed25519 signatures. It supersedes the mixed 0.3.2 stages;
[their evidence](../native-memory-v0.3.2/README.md) remains historical.

[manifest.json](manifest.json) pins all seven wheel names, sizes, SHA-256 digests and public
download URLs. [SHA256SUMS](SHA256SUMS) also pins the manifest. Private source, model weights,
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

## Download the signed release

The release contains 18 assets: seven wheels, `manifest.json`, `SHA256SUMS` and nine detached
signatures. The [plugin guide](../../NATIVE_MEMORY_PLUGIN.md#download-verify-and-install-033)
provides commands to download, verify and install them from GitHub.

The compatible application needs `POST /v1/native/primitive`, unique source versions, a 32-record
cap and refusal of conflicting source rows. Use Aimee 1.0.0 or the exact qualified application image
recorded in the manifest. The plugin release does not include model weights or the GPU runtime.

```sh
python3 docs/releases/native-memory-v0.3.3/verify_assets.py /path/to/downloaded-release
```

All nine detached signatures are required. The pinned Ed25519 public key is preserved;
`--hashes-only` is an artifact identity check and does not verify a release for installation.

[Release notes](RELEASE_NOTES.md) and the [plugin guide](../../NATIVE_MEMORY_PLUGIN.md)
describe the published payload, enrollment and supported execution.
