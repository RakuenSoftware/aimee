# Memory and learning cross-process boundaries: residual work

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`memory-learning-and-inference-boundaries.md`](../done/memory-learning-and-inference-boundaries.md)

## Delivered baseline

Memory, learning, skills, response-composition, and KB-synthesis have explicit source owners and
descriptors, with working storage, recall, ranking, learning, and composition providers.

## Remaining deliverables

- Run memory as a separate dependency-sink program with only its narrow ingest/recall event surface.
- Route learning, skills, and response-composition through their declared cross-process contracts.
- Prove canonical-change, provenance, readiness, and failure semantics across the bus boundary.
- Preserve memory-crossing latency through batching/streaming and publish the measured budget.
- Demonstrate that learning and procedural skills materially affect a production-shaped round trip.

## Completion evidence

Integration fixtures must reject direct feature-module calls into memory, test provider failure and
restart, and compare cross-process semantic output with the accepted in-process compatibility path.
