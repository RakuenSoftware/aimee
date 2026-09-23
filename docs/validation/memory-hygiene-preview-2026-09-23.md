# Bounded read-only hygiene preview — 2026-09-23

`POST /v1/memory/hygiene` forwards to the shared Go owner `memory.hygiene` action.
The request requires `dry_run:true` and one explicit structured shared scope,
for example `{"type":"project","value":"example"}`. Unknown arguments, mutation
options, arbitrary SQL, private placement and all-scope expansion are rejected.

The first detector compares exact content bytes in a current, visible scoped
window. Its default limits are 64 rows and 16,384 content bytes; maximum limits
are 128 and 32,768. One metadata lookahead detects remaining rows. Content is
bounded before crossing the database bus, and a two-second context limits the
query. Row limits bound returned candidates, not physical index work.

A single SQL snapshot supplies payloads, owner, generation and exact revisions.
The Go owner emits stable duplicate-group fingerprints, content commitments and
expected versions, without returning raw memory content. Findings are explicitly
`candidate_only` and propose review. A row or byte cutoff reports partial coverage
and unvisited content; an empty bounded window never proves the collection clean.
Owner failure returns unavailable instead of a successful empty scan.

This path issues SELECT only and performs zero canonical or proposal writes.
The public HTTP adapter transports requests and complete owner replies; it owns
no detector or mutation policy. Native tests cover argument preservation, exact
integer replies and unavailable/error propagation. PostgreSQL tests run through
an actual non-owner runtime role, checking current visibility, expired/archived
and hidden exclusions, stable findings, budgets, version changes and unchanged
canonical data. The [full race suite](memory-hygiene-preview-2026-09-23/full-race.txt)
passed in 121.839 seconds; the [export build](memory-hygiene-preview-2026-09-23/export.txt)
also passed. Fresh deployment tests are pending.

MR-14 remains open: proposal persistence and rejection deduplication, narrow worker
roles, resumable jobs, scheduling, model detectors, task-projection cleanup and
review application are not implemented by this preview. The candidate thinclient supports
`aimee memory hygiene --scope project:example --dry-run --json`, with optional
`--max-rows` and `--max-content-bytes`. It preserves scope values containing
colons and refuses duplicate flags, missing dry-run, unsupported options and
malformed numbers. Native marshalling/transport tests pass; fresh CLI validation
is pending. MCP and private hygiene surfaces are not claimed. Released CT100 remains on 0.4.5.

## Fresh transport failure and repair

The first linked T2 candidate `13191c410` failed its positive hygiene tests: KB
action dispatch adds a `method` field, which the strict domain argument allowlist
rejected. The [failed evidence](memory-hygiene-preview-2026-09-23/initial-failed-T2/T2/shared-memory.json)
is retained. Mutation refusals, canonical immutability and outage refusal passed;
this was not a passing deployment matrix.

Strict hygiene and receipt-verification handlers now validate and remove only a
matching method and optional protocol version 1 before validating domain fields.
Unsupported metadata and domain extras still fail. Actual non-owner replay now
includes the KB transport envelope. The fresh matrix checks direct KB hygiene
early, as well as Server HTTP and candidate CLI paths. Repaired-image results
remain pending.

The transport-envelope repair passed its PostgreSQL/runtime-role and strict
handler race tests in [110.796 seconds](memory-hygiene-preview-2026-09-23/envelope-race.txt).
The preceding candidate `ed2a7ae1d` separately passed its KB-free T3 suite;
T3 does not exercise the KB hygiene path and cannot certify the repair.
