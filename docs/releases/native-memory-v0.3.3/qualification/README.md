# Final 0.3.3 native-model smoke qualification

On 6 October 2026, all five models passed seven short native-memory checks each with shared-runtime
0.3.3 build 5, SHA-256 `a6ddd22b908b8195e51e7a65953fd66f8987c9eb7944ed67bc5f6fcb14ed4511`.
[Results](results.json) and the individual reports identify the tested model versions and GGUF paths.
All model weights came from existing storage on .254. Test data and caches also lived on .254.
Each model used one RX 7900 XTX with zero CPU weight offload.

E2B, E4B and 12B used UD-Q4_K_XL; 26B used Q3_K_M; Qwen3.8 27B used Q4_K_M.
The application was the pinned testing image recorded in the results, before the documentation-only
merge of PR #3013. Validation of the image built from that merge is a separate pending rerun.

Each smoke withdrew admission during initial preparation, denied unauthenticated inference and
unauthorized source selection, stored an authorized canary, recovered it through actual native
cold and warm recall, and verified warm bank reuse. These are smoke checks. Earlier extended
lifecycle matrices retain their original version and hardware limits.

Qwen's first build-4 startup failed because the text-only GGUF adapter selected a multimodal
architecture. Build 5 fixes that mapping. A 128 MiB device budget then refused the approximately
148 MiB recurrent state; a 256 MiB budget passed. The installed stack used the Triton recurrent
fallback, and emitted a tokenizer regex warning. The 26B loader emitted matrix compilation errors
before its fallback completed and recall passed. These runs establish working recall, not warning-free
loading, optimized kernels or a performance benchmark.
