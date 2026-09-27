# MR-03 final closeout — 2026-09-24

MR-03 is complete against its six unchanged acceptance gates. MR-01 through
MR-03 are complete (3 of 18); MR-04 is next. The frozen 123-clause inventory
is unchanged.

## Result and boundaries

Go projection and outer packing count complete serialized UTF-8 JSON and trust
wrappers, preserve whole retained rows and bind projection/selection digests to
source versions. Reviewed procedures render once in their reviewed envelope.
Hard rules reserve space before optional evidence; dropping task-required evidence
changes sufficiency instead of leaving a complete claim attached to an old plan.

History folding retains user, system and developer messages verbatim and in order.
Optional history and generated notices remain assistant evidence. Exact comparison
rejects changes to protected messages and authority; mixed user/tool messages stay
whole. Notices cannot invent an assistant prefill, enter the frozen prefix or
split a tool pair. Native reductions now use the gateway's existing shrink and
structural admission. Full-prefix cache and deterministic projection checks pass.
Automatic observations remain soft guidance. Model extraction cannot replace or
duplicate an existing hard rule. Existing hard rules retain their classification
without guessing historical authorship. No schema migration is introduced.

Final provider serialization obeys the intersection of caller and operator byte
ceilings, including literal zero, retries, fallback and asynchronous native runs.
Unsupported token caps and token reserves explicitly refuse before dispatch;
legacy bytes/4 values are unavailable token-count provenance, not an exact or
conservative hard bound. This does not introduce an exact provider tokenizer or
claim priced cost-saving proofs. External CLI backends cannot expose their final
provider serialization and therefore refuse declared hard limits. Native HTTP
adapters still use the complete-body admission gate.

## Final verification

Application: `577d284d4`; harness: `0bce3ce54` (fixture routing correction only).
One immutable application image:
`sha256:d3bf89373e0859f310ac48934f02bf4b423bdf376d3fe4ca37c05bed452bd355`.

| Fresh process | Passed checks | Exit |
|---|---:|---:|
| T2: Server with optional KB | 1,156 | 0 |
| T3: Server without KB | 676 | 0 |
| Total | 1,832 | |

[Process exits](memory-mr03-final-evidence-2026-09-24/process-exits.json), complete
fixture JSON, [nine image identities and three actual 32,768-byte caps](memory-mr03-final-evidence-2026-09-24/image-identities.json)
and [fixture cleanup](memory-mr03-final-evidence-2026-09-24/cleanup.json) are retained.
The native fixture forces a real fold through five tool calls, captures complete
protected user bytes at the provider, verifies summary provenance and refuses both
external backend types under declared limits. Its external command is `/bin/false`. The fixture explicitly enables server-side
delegation and uses ACP for the provider-CLI case, avoiding Claude-to-tmux
normalization. The initial harness stopped at a routing exclusion; the corrected
harness reaches both execution fences without changing application code. The
[initial failed receipts](memory-mr03-protected-2026-09-24/native-fixture-routing-red.json)
retain the routing failure in both placements.
Provider fixtures verify zero token caps and zero reserves refuse without a send.

The [memory race suite and exported owner](memory-mr03-protected-2026-09-24/memory-race-export.txt)
passed in 300.010 and 5.122 seconds. That run tested `6ef3a6d8f`; its memory Go
owner is identical to the final image. The final
[economizer race suite](memory-mr03-protected-2026-09-24/economizer-tail-race.txt)
passed in 3.672 seconds. [Native fence tests](memory-mr03-protected-2026-09-24/native-fence.txt)
and [all 77 final lint checks](memory-mr03-final-evidence-2026-09-24/lint.txt) passed.
Earlier red reproductions and implementation details are in the
[component report](memory-mr03-protected-2026-09-24.md).

The [six-gate checklist](../proposals/pending/memory-reliability-03-closeout.md)
maps unchanged acceptance clauses to this evidence. These are isolated/local
validation results; they do not assert GitHub CI or completion of later proposals.

## Released installation

All candidate image/process work ran on CT109. CT100's released server, database
and embedder remain [healthy on 0.4.5](memory-mr03-final-evidence-2026-09-24/production-045-health.txt)
after the 0.4.4 upgrade. Work is pushed through the paired local Aimee thinclient.
PreToolUse remains temporarily disabled as requested.
