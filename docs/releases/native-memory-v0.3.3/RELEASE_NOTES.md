# Native memory 0.3.3 candidate

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
all assets before installation. Qualification is bounded to the documented profiles; signing,
publication and installation from public downloads remain separate release gates.
