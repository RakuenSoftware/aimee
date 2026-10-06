# E2B and E4B release candidates

These two additional model plugins use the existing `aimee-native-runtime==0.3.2`.
The opaque wheels are included here for review at the user's request. Private
source, build inputs, model weights and credentials are excluded.

The current build-4 wheels include the dedicated small-model GGUF mapping
and BF16 loading for the small replicated per-layer projections.
Build 2 was replaced after its mapper omitted per-layer embedding weights.
The underlying Rust libraries are unchanged and passed CPU model/adapter
lifecycle checks, 42 per model. The installed
Rust adapters publish and admit native banks and map live positions; the existing
Transformers CPU attention harness reads them. Installed identity and checkpoint
checks, shared-K/V capture tests and wheel audits also passed.

See [validation scope and results](VALIDATION.md) and the named-verdict JSON reports.
The wheels are unsigned local candidates. Actual vLLM serving qualification and
release failure testing are in progress. These results do not qualify that serving
backend or replace the larger models' GPU qualification.

The existing signed five-wheel manifest and verifier remain the manifest for the
original three-model candidate. These additional files are not signed members of
that manifest. Final signing and release metadata must cover the selected final
bytes before publication.
