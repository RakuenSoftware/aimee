# Native memory 0.3.3

Five dedicated adapters share one native runtime: Gemma4 E2B, E4B, 12B, 26B A4B and
Qwen3.8 27B. The release payload includes shared-runtime build 5 and the pinned GGUF loader.
Qwen's text-only Q4 GGUF path now preserves its causal architecture and binds native preparation
to the exact tested checkpoint checksum.

All five passed cold and warm native-memory smokes on one RX 7900 XTX with no CPU weight offload.
Tested profiles are E2B/E4B/12B UD-Q4_K_XL, 26B Q3_K_M and Qwen Q4_K_M. Execution requires
Linux x86-64, CPython 3.12, glibc 2.39+, vLLM 0.30.0 and the tested GPU runtime. The tested mode
is single-GPU, text-only, eager synchronous V1 serving with prefix caching disabled.

The compatible Aimee application supplies authorized native source export. Model weights, vLLM,
Aimee binaries and private source are separate from these artifacts. Verify the signed manifest and
all assets before installation. All seven wheels, the manifest and checksums have detached Ed25519 signatures.
Use the [download and installation guide](https://github.com/RakuenSoftware/aimee/blob/testing/docs/NATIVE_MEMORY_PLUGIN.md).
Qualification is bounded to the documented profiles; model weights and the GPU runtime are supplied separately.

The release tag contains only these release notes and the license. GitHub's automatic ZIP and
tar archives contain no Aimee application repository source or plugin build sources. Install the
signed wheel assets, not those documentation archives. The wheels preserve their reviewed
Python/Triton runtime bridges and compiled native modules; this is not a claim that every wheel
member is compiled machine code.
