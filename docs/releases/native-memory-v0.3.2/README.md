# Three native-memory plugins need a separate 0.3.2 release

The proposed `native-memory-v0.3.2` prerelease delivers Gemma4 12B, Gemma4 26B A4B and Qwen3.8
27B plugins on one shared native runtime. The candidate is version **0.3.2 build 2**. Its five
wheel files have been copied to a local asset stage and checked against the reviewed candidate
hashes. **The new payload is signed, verified and unpublished.** The earlier signed
[0.2.3 stage](../native-memory-v0.2.3/README.md) preserves different bytes and different coverage.

[manifest.json](manifest.json) fixes the five asset names, SHA-256 digests, sizes, proposed URLs,
platform requirements and server prerequisite. [SHA256SUMS](SHA256SUMS) additionally pins the
manifest. The public repository consumes opaque release artifacts; private implementation and
build inputs remain outside it. Model weights, credentials and private signing material are excluded.

## One runtime serves three dedicated adapters

| Wheel package | Role |
| --- | --- |
| `aimee-native-runtime==0.3.2` | Common enrollment, source selection, supervision and vLLM integration |
| `aimee-gemma4-12b==0.3.2` | Dedicated Gemma4 12B Rust adapter and command |
| `aimee-gemma4-26b==0.3.2` | Dedicated Gemma4 26B A4B Rust adapter and command |
| `aimee-qwen3-8-27b==0.3.2` | Dedicated Qwen3.8 27B Rust adapter and command |
| `vllm-gguf-plugin==0.0.5+triton` | Pinned optional GGUF loader for Gemma |

The model packages pin the shared runtime exactly. Their `[gguf]` metadata selects the loader by
version; install with `--find-links` pointing to the downloaded release directory. Package-index
publication is separate. See the [plugin guide](../../NATIVE_MEMORY_PLUGIN.md) for enrollment,
checkpoint binding and execution limits.

## Source export now removes overlaps and rejects conflicts

At inspected application commit `fbc8fe8cd2dbdcbc402aba1d9b3176412417a394`,
`src/server/server_api.c` concatenates identity, preference, active-context and commitment sections
without deduplication or a 32-record cap. Overlapping sections can repeat the same record; the
plugin rejects duplicate source identities. A route that works with an empty store can therefore
fail with an ordinary populated store.

The application follow-up now uses [server_native_primitive_rows](../../../src/server/server_native_primitive.c)
to emit unique immutable source versions and cap the selected view at 32 records. It checks the
complete authorized view before truncating, so conflicting versions or content outside that window
also refuse export. The ordinary recall path and authorization remain unchanged.
[Regression tests](../../../src/tests/test_server_native_primitive.c) cover overlapping sections,
exact large IDs, different owners, revision/content conflicts, the cap and malformed views.

The candidate's enrolled validation used an isolated application baseline `4e822e6ee` with the
recorded export repair. The follow-up supplies the same required behavior with complete-view
conflict validation. Final application image qualification and publication remain separate from
these projection tests and from the recorded GPU results.

## The candidate has execution evidence within a narrow profile

The reviewed delivery report records 21 actual enrolled lifecycle checks per final plugin,
87 serving-bridge tests without skips, Qwen 324/324 recall cases, co-installation/uninstall isolation,
and actual deployment, rollback and restoration. Gemma's earlier shared-runtime runs each passed
25 lifecycle checks. These counts refer to separate gates; they are not summed into one score.

The reviewed 281-member evidence archive has SHA-256
`dbc5439e151055b5a7494a0f1e3086087e684e4a25673173ab32754b25bf58f3`.
Its independent audit reports every member digest verified and candidate wheels matching deployment
evidence. The public asset review checks those wheel identities and installer metadata; it does
not rerun GPU execution. The [promotion audit](../../validation/pr-3003-release-audit-2026-10-05.md)
records the inspected sources and checks.

Execution coverage is Linux x86-64, CPython 3.12, glibc 2.39+, vLLM 0.30.0 and single-GPU CUDA
text serving with eager synchronous V1 and prefix caching disabled. Tested checkpoints are Gemma
12B Q4_K_M, Gemma 26B A4B Q3_K_M and Qwen 27B FP8 with CPU matrix offload. The 0.2.3 Q8/HIP
observations belong to the older candidate. No public-index install or public-release download is
established by the new candidate evidence.

## Finish the delivery in this order

1. Merge the source-export repair, qualify the final application image, and publish
   the compatible application through its separate approval gates.
2. Verify all 14 staged assets with the default verifier. The controlled pipeline signed each
   of the five wheels, `manifest.json` and `SHA256SUMS` with the reviewed Ed25519 key; all seven
   detached signatures verify. Keep that verification after any transfer.
3. Run [create_draft_release.sh](create_draft_release.sh) with the signed asset directory and exact
   reviewed application commit. It verifies the bytes and signatures before creating a draft
   prerelease with [RELEASE_NOTES.md](RELEASE_NOTES.md), with Latest disabled.
4. Publish after review, download all assets afresh, verify signatures, and install each plugin in
   an independent Python 3.12 environment from public downloads. Check GGUF dependency resolution,
   `pip check`, enrollment, cold/warm recall, updates and revocation against the published server.

To verify the signed stage before creating a draft:

```sh
python3 docs/releases/native-memory-v0.3.2/verify_assets.py /path/to/release-assets
```

The verifier requires all seven signatures. Optional `--hashes-only` mode checks identity alone
and cannot satisfy the draft-release script. Retain the trust-key review if the signing
pipeline changes keys; a key bundled with downloaded bytes alone does not establish their origin.

## Additional small-model candidates

[E2B and E4B candidates](small-model-candidates/README.md) add two dedicated model
plugins on the same shared runtime. Their wheels and bounded CPU evidence are
included for review. Serving qualification is in progress; they are not signed
members of the existing three-model release manifest.
