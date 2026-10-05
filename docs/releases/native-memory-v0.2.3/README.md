# Native-memory executable release v0.2.3

The newer [0.3.2 release preparation](../native-memory-v0.3.2/README.md) covers three dedicated
model plugins on one shared runtime. This signed stage preserves the earlier candidate and its
validation; use the current [plugin guide](../../NATIVE_MEMORY_PLUGIN.md) when choosing a release.

This change reviews the **compiled Aimee vLLM executable distribution** for
the `native-memory-v0.2.3` GitHub Release. The implementation source and build
pipeline remain private. The GitHub Release in `RakuenSoftware/aimee` will
attach these exact files:

| Asset | SHA-256 | Size |
| --- | --- | ---: |
| `aimee_vllm-0.2.3-1-cp312-cp312-linux_x86_64.whl` | `b4b9d7fe7249bf304cadabbfea0bde53bcdd84d8f7186e3a9aa74180f9380736` | 3,526,478 bytes |
| `vllm_gguf_plugin-0.0.5+triton-py3-none-any.whl` | `09f2fb8b5f22a1f7b1bc965948b7d6085136ae1084876f15a1c1740c63aa9656` | 230,630 bytes |
| `manifest.json` | `7fc6eed5eb09e9de856e92ca927b439f90a7b53be911b2f84531a82c0b4e2977` | 2,056 bytes |

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
Keep this separate from Aimee's application releases and
do not mark it “Latest.” A clean external installation of the plugin wheel
with its `[gguf]` extra must succeed from the public URLs before announcing
delivery.

The exact GLIBC 2.34-compatible candidate passed enrolled end-to-end tests
with Gemma4 12B Q8 GGUF on an RTX 5080, plus Gemma4 26B Q8 and Qwen3.8 27B
Q8 on two RX 7900 XTXs. Each answered both selected facts from one native
bank; prompt-token counts were 34, 34, and 29 respectively. Revocation caused
HTTP 503 on the next request in all three tests. A separate earlier test recovered
three selected records both in one combined answer and in three individual
answers with Qwen. The earlier Qwen native-answer regression was traced to a GGUF
normalization conversion in local preparation and corrected before this wheel
was built. These release-path tests used a local mTLS primitive fixture with
the standard thinclient profile. Separately, the wheel connected through an
actual thin-client enrollment to a fresh `aimee:testing` server, fetched the
native primitive, and received HTTP 401 when the bearer was invalidated. That
fresh server had no authorized memory writes, so this check returned zero
records; selected-record correctness is established by the mTLS release-path
tests above. At staging, public Aimee v0.4.6 predates the primitive route, so
the server must reach a public application release before users on the latest
Aimee can use this plugin. The stage remains a candidate until review and
publication.
