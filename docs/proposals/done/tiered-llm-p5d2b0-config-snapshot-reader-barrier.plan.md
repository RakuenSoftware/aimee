# P5-D2b0 active-config snapshot reader barrier

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** terminal done; implemented and validated.
- **Parent:** `tiered-llm-p5d2b-safe-config-projection.plan.md`.
- **Purpose:** remove a pre-existing C data race before a management endpoint begins copying the
  full active `config_t` on demand.

## Defect and boundary

`config_snapshot_get` reads an ordinary `config_t` from a two-slot double buffer and retries when
the sequence changes. A reader can still be copying old slot 0 while two consecutive publishers
switch to slot 1 and then begin rewriting slot 0. The final retry detects change but cannot undo
the overlapping non-atomic read/write and its undefined behavior.

This slice changes only the active snapshot publication/read primitive and its focused tests. It
does not add management routes, config projection, wire fields, config parsing/saving, reload
semantics, re-appliers, environment behavior, or a new public configuration source.

## Reserved-slot reader-pinning contract

Keep the existing two slots, writer mutex, sequence counter, active index, content token, and
public functions. Add one `_Atomic unsigned` state word per slot. The high bit, derived portably
from `UINT_MAX`, is `WRITER_RESERVED`; remaining bits are a saturating reader count with
`READER_MAX = WRITER_RESERVED - 1`. A separate count plus later zero check is forbidden: it has a
TOCTOU window in which a delayed reader can pin after the writer observes zero but before the
writer makes the sequence odd.

`config_snapshot_get` performs this loop:

1. acquire-load an even sequence and the active slot;
2. load that slot's state and use compare-exchange to add one only while the reservation bit is
   clear; if the unreserved state equals `READER_MAX`, return `-1` immediately with `*out` and the
   state word unchanged; if reserved, restart at step 1;
3. acquire-load sequence and active slot again;
4. if either changed or the sequence is odd, unpin and retry without touching the slot payload;
5. copy the pinned ordinary `config_t` to the caller;
6. release-decrement the slot state and return success.

The publisher, already serialized by `g_snap_wlock`, chooses the inactive slot and acquire-CASes
its state from exactly zero to `WRITER_RESERVED`. If the state is nonzero it yields through an
existing portable abstraction or a portable bounded-spin fallback and retries without holding any
resource a reader needs to validate or unpin. Only after reservation succeeds may it set the
sequence odd or write a payload byte. It writes the complete inactive slot, publishes active
index/token and the final even sequence as today, then release-stores the slot state back to zero.
A later publisher cannot reuse a prior active slot until every reader that validated a pin on it
has released the pin.

A delayed reader and writer contend on the same atomic transition. If the reader CAS increments
first, the writer cannot reserve until unpin. If the writer's zero-to-reserved CAS wins, that
reader cannot pin and never reaches payload. Sequence validation still determines whether a pin
belongs to the current generation; reservation supplies the exclusion for ordinary payload
access. No ordinary slot read overlaps a write. Reload remains rare; bounded writer
spinning/yielding is acceptable, but it must not hold a resource needed by readers to unpin.
Readers remain concurrent and take no mutex.

State words rely on static zero initialization and are never reset while publication is live.
Repeated `config_snapshot_init` uses the same reservation/publication protocol; a test-only reset
may zero state only after joining all readers/writers and proving quiescence. `config_reload`,
content-token no-op behavior, and re-applier ordering stay unchanged. Document acquire/release
ordering at the implementation: successful reader pin has acquire semantics, unpin is release,
writer reservation is acquire, reservation release is release, and existing final even sequence
publication/validation remains release/acquire.

## Tests and gates

- Focused deterministic seams cover both races without timing sleeps: pause after first
  sequence/active observation but before pin CAS and force (a) writer reservation first, proving
  reader cannot pin/touch payload, and (b) reader pin first, proving writer cannot reserve/write
  until unpin. A second seam pauses after validated pin, performs one publication, and proves a
  consecutive publisher cannot rewrite the pinned old slot until release.
- Stress multiple readers copying the full `config_t` while one writer performs thousands of
  alternating, distinct publications. Every accepted record equals one complete published image;
  no mixed tuple is accepted.
- Run the stress under ThreadSanitizer or the repository's equivalent data-race detector. A plain
  functional stress is not sufficient.
- Cover pre-init failure, NULL output with no output touch, same-content no-op reload without
  reservation/re-appliers, repeated init with and without a pinned old slot, multiple-reader
  drain/progress, reservation-bit distinction, counter saturation without wrap/mutation, and
  publication after the last reader drains.
- The saturation seam asserts return `-1`, byte-unchanged output, state exactly `READER_MAX`, and
  distinguishes a reserved slot as retryable contention rather than saturation failure.
- Existing config reload/snapshot tests, production server build, lint, unit and build-integrity
  gates remain green on Linux and supported compile targets.

## Non-goals

No generic RCU framework, heap generations, config schema change, management endpoint, parser
change, or performance redesign. D2b consumes the repaired primitive in the following slice.
