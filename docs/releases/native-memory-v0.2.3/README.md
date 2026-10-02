# Native-memory executable release v0.2.3

This change reviews the **compiled Aimee vLLM executable distribution** for
the `native-memory-v0.2.3` GitHub Release. The implementation source and build
pipeline remain private. The GitHub Release in `RakuenSoftware/aimee` will
attach these exact files:

| Asset | SHA-256 | Size |
| --- | --- | ---: |
| `aimee_vllm-0.2.3-1-cp312-cp312-linux_x86_64.whl` | `2a0445c7cf70f80035e5f719e1ca87ee1a4246fad930e3236dc17077e0e59b5a` | 3,443,215 bytes |
| `vllm_gguf_plugin-0.0.5+triton-py3-none-any.whl` | `09f2fb8b5f22a1f7b1bc965948b7d6085136ae1084876f15a1c1740c63aa9656` | 230,630 bytes |
| `manifest.json` | `782b1b997a2be9bf3a83030a4907c242dda4f9c21462f2073d8ebb005ef8b331` | 1,562 bytes |

The plugin wheel contains the `aimee-native` Rust executable, compiled local
encoder modules, the portable native consumer, and the vLLM plugin. It contains
no model weights or private implementation source. The GGUF loader wheel is a
pinned optional serving dependency. Aimee server/CLI binaries and model weights
are not bundled into either wheel.

The public repository is intentionally only the release-verification boundary.
It does not fetch, clone, identify, or build from the private implementation
repository. The release assets are produced and reviewed in the private build
pipeline, then exported as opaque signed artifacts. This PR verifies only the
identity and integrity of those exported bytes.

`manifest.json`, `SHA256SUMS`, and their Ed25519 signatures are committed here
so the pull request fixes the intended release bytes. The two wheel files and
their signatures belong in GitHub **Release assets**, not Git history. Before
uploading, run `python3 verify_assets.py ASSET_DIRECTORY` against the staged
files. Run the same verification after downloading all eight assets from the
published release.

The release requires an Aimee server with the enrolled-client
`POST /v1/native/primitive` route. That server change is reviewed separately
in [PR #3001](https://github.com/RakuenSoftware/aimee/pull/3001); publish
the executable after it is in the version offered to users. The 0.2.3 wheel
uses the standard Aimee thinclient enrollment and reads its current profile
directly. It does not need a separate plugin account or server-side model
download.

After review, run [create_draft_release.sh](create_draft_release.sh) with the
staged asset directory and the exact reviewed Aimee commit. It verifies the
payload, then creates a draft `native-memory-v0.2.3` prerelease in the Aimee
repo using [RELEASE_NOTES.md](RELEASE_NOTES.md). It attaches the two wheels,
their two `.sig` files, `manifest.json`, `manifest.json.sig`, `SHA256SUMS`,
and `SHA256SUMS.sig`. Do not attach the local staging receipt.
Keep this separate from Aimee's ordinary `v0.4.x` application releases and
do not mark it “Latest.” A clean external installation of the plugin wheel
with its `[gguf]` extra must succeed from the public URLs before announcing
delivery.

The exact plugin wheel passed an enrolled end-to-end test on an RTX 5080 with
Gemma4 12B Q8 GGUF: both selected facts were answered with 34 prompt tokens,
and access revocation caused HTTP 503 on the next request. Qwen3.8 27B Q8
GGUF-only preparation produced a complete bank. Its live GGUF consumer run
remains a separate validation item because its 28 GB checkpoint cannot run on
the 16 GB 5080 and the RX 7900 XTXs were occupied during this candidate test.
