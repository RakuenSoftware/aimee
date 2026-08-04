# Embedder sidecar deployment and model-migration residual

- **State:** PENDING — residual scope only.
- **Archived parent:**
  [`embedder-image-split-and-rebuild.md`](../done/embedder-image-split-and-rebuild.md).

## Delivered foundation

The two baked embedder sidecar images, guarded publishing, stunnel mTLS proof, KB-issued identity,
query/document polarity, serving-identity guard, and curator replay on embedder or synthesis identity
change are shipping.

## Remaining work

- Make managed deployment select `aimee-embedder-a25m`, `aimee-embedder-nomic`, or no bundled
  container; remove torch and embedded weights from the KB images and retire contradictory in-process
  topology text.
- Route the KB through the sidecar with its client identity and prove missing/invalid identity fails
  loudly.
- Measure retrieval latency before accepting the hot-path HTTP/mTLS hop.
- Turn same-dimension model/pooling/prefix changes into a supported, attended migration rather than a
  fresh-DB-only procedure; preserve an untouched rollback store until verification passes.
- Align dimension-change, same-dimension change, and synthesis-change warnings across the wizard,
  CLI, dry-run plan, and operator documentation.

## Acceptance

Managed E2E covers all three deployment choices and both model-change classes. It proves vector-space
identity, data-loss warnings, guarded rebuild/replay, restart stability, rollback, and no silent
lexical or in-process fallback.

