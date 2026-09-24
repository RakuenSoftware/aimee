# MR-01: relation intervals use the common validity clock

Relation search, entity edges and entity-profile aggregation checked parent
eligibility but omitted the relation's own interval for current reads. The
historical relation adapter compared timestamp strings, so equivalent instants
with different offsets did not match. A [packaged restricted-role replay](memory-mr01-relation-validity-2026-09-24/before-race.txt)
reproduces future/expired relation admission and the historical offset failure.

All three current serving queries now enforce inclusive `valid_at` and exclusive
`invalid_at` boundaries before ordering, limits or aggregation. Requested-time
search uses the same timestamp normalizer. Public historical queries reject
relative or malformed times. Parent authorization, lifecycle and producer-input
checks remain mandatory; this does not introduce historical reconstruction of
an unavailable parent. The eligibility fingerprint advances to
`current-validity-v10` so cached policy observations cannot stand in for it.

The [targeted race suite](memory-mr01-relation-validity-2026-09-24/after-race.txt)
passes in 1.460 seconds, including non-UTC sessions, offset/precision boundaries,
malformed stored times, higher-ranked invalid rows and public adapters. An older
public test expected an already-expired relation in current edges; it now
requires only the current edge, while its historical query still returns the
old edge. The [HTTP reproduction](memory-mr01-relation-validity-2026-09-24/http-before.json)
on `50282c8ba` passes the preceding lifecycle/derived fixture and then admits a
future relation through entity edges. The final [full race suite and exported owner build](memory-mr01-relation-validity-2026-09-24/full-race-export.txt)
pass in 285.406 and 5.310 seconds after both producer repairs below.
Corrected HTTP validation is pending. The expanded candidate fixture includes
121 checks, adding briefing and match-explanation coverage to the common
population as well as the relation interval regression.

The [first full suite](memory-mr01-relation-validity-2026-09-24/producer-frame-before.txt) exposed a producer dependency: semantic-frame time hints
such as `sep 12` had been copied into governed relation bounds. The producer now
normalizes parsed absolute timestamps and otherwise retains the parent's bound,
while keeping the original hint in relation text/targets. Existing malformed
stored bounds remain refused until repaired through reindexing. This fixes the
producer rather than relaxing serving-time validation. A regression covers
relative hints, impossible dates, absolute offsets and fractional seconds.

A [second full-suite reproduction](memory-mr01-relation-validity-2026-09-24/producer-token-before.txt)
found tokenized lexical date references (`2026 09 12`) being used as the parent's
fallback bound. That inference is removed: without an explicit parent start,
the producer retains the parent creation timestamp instead of promoting a
retrieval token into temporal authority.
