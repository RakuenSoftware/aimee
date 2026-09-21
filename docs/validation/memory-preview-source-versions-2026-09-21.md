# Memory preview source versions

Automatic shared-memory previews now bind the exact canonical memory or selected
summary revision, including the summary's canonical parent revision. A generated
headline can change independently of its parent. Shared schema 31 assigns summary
revisions on insert/update, preserves no-op revisions, rejects identity changes,
and prevents callers from assigning their own counters.

The Go diagnostic ingress path rehydrates selected canonical rows, the same
legacy-selected headline, and their versions in one statement snapshot under
current eligibility, RLS and any explicit scope. Ranking remains a separate
candidate step. Missing or newly ineligible selected rows refuse the projection.
This replaces the former per-row epistemic queries plus public metadata query
with one metadata/source query (six calls to one for five selected memories).
Non-ingress public diagnostic record shapes and tracing stay unchanged.

The projection commits the actual rendered row text, including its escaped,
clipped key/preview and owner-formatted three-decimal score. An explicit score
string survives cJSON floating-point round trips. Compatibility content/preview
fields remain available but unused full content is not claimed as rendered text.
Decimal source IDs and revisions remain exact across the external C host.

Go verifies the commitment before outer packing and retains only selected rows'
source references. The host forwards the opaque commitment and emits Go-selected
references only after integrity acceptance. The existing Go release handle now
includes these sources. One owner SQL statement checks canonical, summary,
episode and assertion revisions and eligibility before provider-byte selection.
The canonical eligibility expression is shared rather than repeated for each
source kind. Legacy unversioned rows remain explicit compatibility inputs.

## Validation

- Full Go/PostgreSQL memory and runtime-web suites pass (43.712 seconds).
- Runtime-role and source/assembly race regressions pass (114.341 seconds).
- Summary and episode upgrade regressions cover preserved payloads, repeated
  migration, no-op/forged counters, independent field revisions and immutable IDs.
  A fresh database upgraded from schema 30 to 31 twice preserves canonical rows,
  mutation receipts, evidence history and authored summaries. Edited summary
  revisions survive another schema reapply; restricted ACLs remain intact.
- Runtime-role tests cover independent summary edits, deletion/reparenting,
  canonical fallback edits, suppressed/expired/hidden parents, explicit-scope
  refusal, exact large IDs and refreshed sources through the public command.
- Assembly tests cover transport rounding, mutated content/source commitments,
  missing/null commitments, partial packing and exact release-cache references.
- The actual C ingress adapter with the separate Go fixture preserves a summary
  ID above 2^53 and the rendered score. It validates all three provider routes
  with both fence modes; stale/unavailable/mismatched answers select zero bytes.
- Native build, independent export, 17 S1 tests, all 77 lint checks, documentation,
  proposal links and the memory/C ownership boundary pass.
- Ten authenticated fresh-deployment checks exercise rendered commitments,
  summary/parent and fallback versions, scope changes, no-op revision preservation,
  independently edited summaries and refreshed bindings. Fresh application/harness
  `d8154014bb85f0bd62e130f5b5ecf1a328f4cc93` passes **1,688/1,688 checks**:
  [103 T1](memory-preview-source-versions-2026-09-21/fresh-t1-d8154014bb.json),
  [993 T2](memory-preview-source-versions-2026-09-21/fresh-t2-d8154014bb.json), and
  [592 T3](memory-preview-source-versions-2026-09-21/fresh-t3-d8154014bb.json).
  All [12 image identities and four actual provider caps](memory-preview-source-versions-2026-09-21/image-identities-d8154014bb.json)
  were verified. The application image is
  `sha256:5123280fcc4d8c501564e26cd9e77c434e45ba05aaf43973433091b33037d455`.
  All [12 owned containers and 12 empty networks were removed](memory-preview-source-versions-2026-09-21/cleanup-d8154014bb.json),
  preserving images, volumes and raw receipts.
  Latest-push CI is pending; the preceding source-release commit has a complete
  [green CI run](https://github.com/RakuenSoftware/aimee/actions/runs/35589821082).

Local [Go suite](memory-preview-source-versions-2026-09-21/go-tests.txt),
[race suite](memory-preview-source-versions-2026-09-21/race-tests.txt), and
[schema-upgrade](memory-preview-source-versions-2026-09-21/schema-upgrade.txt)
results are retained alongside the fresh receipts.

The existing 36-assertion/36-parent source-check benchmark remains one query,
16 allocations, with median **1.34 ms** and **56,682 allocated bytes** across three
runs. [Raw samples](memory-preview-source-versions-2026-09-21/source-check-benchmark.txt)
are retained. This is a component benchmark, not whole-request P95.

This extends source coverage; it does not complete durable prepared/dispatch
receipts, transitive/collection freshness or locking through remote handoff.
Private recall, code and unversioned learning/procedure channels remain outside
this source-version check. None of MR-01–MR-18 is certified complete here.
Memory policy and module-side transport remain Go. The existing C bus stays C.
