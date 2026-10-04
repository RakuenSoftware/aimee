# Documentation and CI audit for the 1.0.0 release work

The application release target is **1.0.0**. This audit compares the maintained guides with
`d1e31ad1eca51361e80e1fb0bdf71031eb3a871d`, which contains the replaceable-memory implementation
and the integrated `testing` tree. The documentation changes prepare that release; they do not
publish it. Independent module pins and the native-memory plugin retain their own versions.

## Scope and method

The inventory covers 911 tracked documentation files, including proposals and validation history.
A narrower set of 158 maintained guides, indexes and engineering documents was searched for
release claims, obsolete ownership, missing source paths, implementation status and diagram drift.
Findings were checked against the release API, process descriptors, runtime source, browser code,
workflow definitions and existing validation reports. This is a source-guided audit, not a claim
that every historical paragraph received a new behavioral test.

Current guides describe the implemented boundary. Proposals and dated reports preserve their
original findings and measurements; a later resolution does not rewrite an earlier failure as a
pass. Unrelated local drafts, benchmark experiments and nested worktrees are outside this change.
The clean validation checkout contains the tracked tree plus this change's new files.

## Corrections

| Finding | Result and source boundary |
| --- | --- |
| Release notes denied the intended 0.3.0 release and lagged publication | README, release notes, status and release guidance identify the intended [0.3.0 release](https://github.com/RakuenSoftware/aimee/releases/tag/v0.3.0), published 2026-08-04; [0.4.6](https://github.com/RakuenSoftware/aimee/releases/tag/v0.4.6) is the previous published baseline. The source series and helper target 1.0.0. |
| README described an earlier product | Rewritten around persistent runtime state, standalone personal Server, optional shared KB, governed actions, replaceable memory and native attention delivery. |
| Architecture and process descriptions lagged extraction | Updated process ownership and diagrams: native resource hosts and event buses, 26 Go process identities, Go PostgreSQL provider, separate workflow and WORM peers. Descriptor presence does not imply every process runs in every placement. |
| Memory review was described as missing | Current Memory Center implements correction review for both placements; control-web typed-fact review remains a separate surface. Checked against `frontend/src/pages/Memory.tsx`. |
| Memory engine replacement and model delivery were conflated | Contract and diagrams distinguish native/Cognee retrieval from the separately staged vLLM attention plugin. Aimee retains canonical identity, authorization and lifecycle. |
| Upgrade instructions promised removed layout adoption | Current startup does not rename legacy databases or repair old role layouts. Filesystem migration of an already compatible PG18 layout is distinct from logical database conversion. |
| Model deployment still implied application-bundled inference | Standard compositions use separate embedding and optional synthesis sidecars; model identity and vector-space migration requirements remain explicit. |
| Readiness always required a KB | Standalone Server can be ready without a configured KB. Configured KB dependencies gate readiness; scoped semantic probes remain necessary. Checked against `src/server/server_ready.c`. |
| Module documents referenced deleted paths or invented provider implementation | Corrected source owners and retained native adapters. The roundtable provider declaration alone is not evidence of registered execution. |
| Generated environment reference omitted Go-only settings | Generator now scans production Go owners as well as native sources, excludes test files and fixtures, and rejects concatenated environment prefixes as individual variables. Critical backend and deployment wiring is documented. |

Six SVG figures cover the overview, processes/storage, deployment, memory backends, erasure and
native attention path. `docs/images/architecture/render.py` is their authoring source. The memory
module document uses inline Mermaid so independently exported documentation has no new image-file
dependency. The figures were rendered with `rsvg-convert` and visually checked.

## CI findings and fixes

The initial [CI run](https://github.com/RakuenSoftware/aimee/actions/runs/37213509038) passed the
required adapter race and real Cognee contract job, native test shards, sanitizers and deployment
checks. These distinct failures required corrections:

| Failure | Correction |
| --- | --- |
| Memory ownership ratchet rejected new transport callers | Reviewed reset-derived host calls, host fixtures and existing Vault first-boot token custody; retained exact symbol and source-digest enforcement. No native memory fallback was admitted. |
| Generated configuration was stale | Regenerated the reference from current native and Go sources. |
| Native formatting failed | Applied the required clang-format 19 to the four changed C transport files. |
| Frontend gate regression rejected its success case | Its environment omitted the new required memory job. Both frontend and memory outcomes now receive success/failure/cancelled/skipped coverage. Required workflow checks remain enforced. |
| Frozen reliability inputs differed | Reviewed three changed test files: two call the exported `ValidFor` spelling; one adds nil-receiver refusal coverage. Updated only those digests and recorded the review in the manifest. Existing groups, cases and required outcomes remain unchanged. |

Local script validation also exposed two proposal-ordering fixtures that assumed Git's global
initial branch was `master`. Their temporary repositories now explicitly create that branch;
the chronological ordering checks are unchanged. The module-documentation catalog also retained
the retired `db2` entry, while the memory backend pointer introduced a non-canonical document
inside `docs/modules`. The catalog now matches the 34 descriptors; the pointer is removed and its
remaining link targets the complete canonical memory guide. Supplemental memory, KB and PostgreSQL
sections now nest under the canonical module-document schema; the checker passes all 34 documents.

## Local verification

- All 75 lint checks passed in the isolated checkout.
- Final document, generated-reference, module inventory, memory ownership and clang-format 19 checks passed.
- Application source and refactor snapshots were refreshed for release preparation and their checks passed; independent repository versions and published pins were preserved.
- Frozen reliability passed 173 required Go tests and 40 Python checks with no skips. The
  [result artifact](documentation-1.0.0-ci-2026-10-04/memory-reliability.json) records the cases and
  source/diff identities. PostgreSQL used the CI UTC setting; its disposable cluster was stopped
  and removed.
- Every discovered script test passed across the complete run and its proxy-tail replay after
  explicitly building the thin client. The proxy suite passed 28 tests.
- Frontend and memory aggregate-gate regression variants passed.
- Native and Go environment-reference discovery, fixture exclusions and dynamic-prefix rejection passed.
- The release helper proposed `v1.0.0` without creating a tag.

## Qualification limits

A green protocol test does not establish production retrieval quality, model quality, performance
or universal safety. Cognee's first adapter still bounds scopes to 256 eligible records. A deliberate
memory restart can interrupt a request for the existing timeout. See the deployed
[native](memory-native-ct-253-2026-10-04.md) and
[Cognee](memory-cognee-ct-253-2026-10-04.md) reports for measured behavior and cleanup evidence.

Some generated internal settings remain explicitly marked undocumented; their names are visible
rather than silently dropped. Historical proposals remain proposals until their acceptance evidence
and implementation status say otherwise. Current deployment guidance belongs in the maintained
guides, not in a point-in-time validation artifact.
