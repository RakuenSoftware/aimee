# Memory and learning cross-process boundaries: residual work

- **State:** PENDING. Residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`memory-learning-and-inference-boundaries.md`](../done/memory-learning-and-inference-boundaries.md)

## Delivered baseline

Memory, learning, skills, response-composition, and KB-synthesis have explicit source owners and
descriptors, with working storage, recall, ranking, learning, and composition providers.

## Remaining deliverables

- Memory process isolation and its canonical-change/provenance/readiness/restart baseline are implemented; see [the archived G0 closeout](../done/memory-reliability-g0-closeout.md). Remaining scope is the learning/skills/response-composition boundary and measured production effects, not a second memory-process migration.
- Route learning, skills, and response-composition through their declared cross-process contracts.
- Prove canonical-change, provenance, readiness, and failure semantics across the bus boundary.
- Preserve memory-crossing latency through batching/streaming and publish the measured budget.
- Demonstrate that learning and procedural skills materially affect a production-shaped round trip.

## Completion evidence

Integration fixtures must reject direct feature-module calls into memory, test provider failure and
restart, and compare cross-process semantic output with the accepted in-process compatibility path.
