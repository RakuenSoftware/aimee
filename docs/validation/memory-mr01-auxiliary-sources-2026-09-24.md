# MR-01 persistent typed sources and historical process replay

Observations, approved learning procedures and entity summaries now retain an
owner, stable record identity and revision with the exact selected payload.
Summary references also retain every directly copied memory input. Revalidation
checks the current audience, lifecycle, interval and revisions; the durable send
guard protects these tables until explicit completion. User-supplied working
context is ephemeral request input, not a stored memory with a revocation claim.

The packaged runtime needs only SELECT on the new learning revision columns.
The first full replay exposed missing grants; the shipping grant block now
includes them, and the restricted-role replay passes in 232.112 seconds. Its
negative permission checks still prohibit learning writes and evidence_refs
reads. Shared schema 35 refuses older pre-provisioned databases; schema metadata
is recorded after all schema objects and guards.

The authenticated transaction now carries the internal EligibilityContext:
verified principal/transport, narrowed audience, operation purpose, temporal
mode/anchors and owner policy version. Release contexts retain exact observed
root and parent revision generations, rather than inventing a global counter.
Actual release SQL rechecks those observations under the verified audience.
Native hard-rule source metadata stays in the retained references, outside the
model-visible rule text; the original rule bundle remains intact.

The historical process replay passes all five requested-time checks on
51b5f3dbe, including exact authorized current/superseded/archived sets and a
revocation after selection. The fixture records the required semantic evidence
events. Its own project isolates the exact set from other authorized semantic
candidates created earlier in the common fixture. Earlier failures are retained.

[Evidence](memory-mr01-auxiliary-evidence-2026-09-24/targeted-race.txt) includes the
focused auxiliary tests, [restricted-role result](memory-mr01-auxiliary-evidence-2026-09-24/runtime-grants-after.txt),
and [historical process checks](memory-mr01-auxiliary-evidence-2026-09-24/historical-http/checks.json).
The combined memory race suite passes in 224.036 seconds and the exported
C-process build passes in 5.461 seconds. A final 6.776-second focused race replay
checks auxiliary sources, request-context cleanup across pooled connections,
native rule metadata, and every protected table’s bulk-truncate refusal. Links
and scope tags now have direct send guards as well as their revision hooks.
The [final closeout](memory-mr01-closeout-2026-09-24.md) records all final-candidate deployment checks passing.
These component passes alone do not certify MR-01.
