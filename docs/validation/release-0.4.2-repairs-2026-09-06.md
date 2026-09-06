# 0.4.2 repair verification

The fixes pass isolated release qualification on `.253`. The corrected source still needs
its required GitHub CI checks and the existing protected promotion/release workflow before
0.4.2 can be published. The repository's version calculation selects **0.4.2**, following 0.4.1.

This supersedes the [original published candidate verdict](release-0.4.2-2026-09-06.md) for the
repaired source only. It does not declare the old `testing-6083b85` images fixed.

## Scope and behavior

The two stores remain separate. Server memory belongs to the local user and can contain
personal memories and PII. The KB holds shared knowledge across projects and workspaces.
Basic CLI, HTTP, and MCP memory operations default to the user store; `store=kb` selects the KB.
No ID collision, missing local record, unavailable database, or project argument causes a
fallback or a write to the other store. Existing 0.4.1 KB records remain in the KB and are
addressed explicitly after upgrade. No records were migrated between these stores.

The Memory page defaults to Personal (local), distinguishes the stores in references, and
uses local retirement separately from KB reject/restore. Late responses from the previous
store cannot replace the currently selected store's rows. The manual and help describe the
selection and the KB-only historical-read behavior.

## Repaired findings

| Finding | Repair and regression evidence |
| --- | --- |
| R042-1: store/list and get/supersede addressed different namespaces | Consistent explicit routing across API, served CLI argument specs, MCP, and UI. Real colliding IDs exercise store, list, get, search, supersede/update, retirement, and restart. |
| R042-2: 0.4.1 memories inaccessible after upgrade | KB entrypoint upgrades untouched image grants to include memory data stage 5895. Edited operator policies remain intact. Both old global/project records are readable with exact Unicode contents after upgrade and restart. |
| R042-3: parallel workspace fixture failure | Each C binary receives its own HOME/TMPDIR. A regression reproduces competing HOME removal and Git metadata creation; the complete C suites, including workspace, pass under parallel execution. This addresses the shared-directory hazard; local results do not retroactively turn the old CI run green. |
| R042-4: HTTP 200 carrying an application error | Explicit failures remain HTTP failures even when the optional web status provider is disabled. Local database, KB daemon, and KB memory-process outages are exercised with recovery. |
| R042-5: clean content screening refused ordinary KB writes | Serialize the explicit zero `sensitive_status` verdict that the C receiver requires. The wire regression covers both placements, and live KB writes succeed. |
| R042-6: KB list/search omitted requested scopes | Go queries honor project/workspace/global ordering and append other KB scopes for explicit all-scope reads. Exact queries stay exact; runtime RLS remains in force. |
| R042-7: KB memory-process outage looked like an empty successful list | Propagate the failed data-stage result. Regression distinguishes failure from a valid empty list, and live tests pause the memory process while the KB daemon remains up. |
| Long memory reads | The bounded C preview preserves UTF-8; exact get returns the full stored content. Both stores round-trip long Unicode notes. |
| Personal replacement could lose the original on a failed second write | Replace the local row with one atomic SQL update instead of retiring it before inserting a conflicting key. |
| Stale native test deployment | Export descriptors, attach the correct placements, install every companion grant, and use separate runtime/migration identities in fresh databases. |

## CI followup

The first repair CI run caught an overbroad historical-read change: ordinary KB get could
return a retired record. The correction keeps ordinary lookup active-only and passes an
explicit `as_of` selector through both the preview and full-content reads. The unchanged
shared-database retirement assertion and the complete DB2 process replay pass. Both live
stacks now also prove historical content and validity before and after retirement.

Both real LSP providers passed their behavior checks on Linux and macOS. The frozen-source
validator rejected the memory schema changes in shared MCP files. An exact, literal memory
integration manifest now preserves whole-file comparison against the frozen semantic source;
14 validator/benchmark tests pass, including rejection of unrelated LSP or memory drift.
The sanitizer build also exposed missing collapsed-family linkage in two flat-schema fixtures;
they now provide an assertion that the unused branch is never executed. Updated CI remains
required; these local corrections do not turn the first run green retroactively.

## Verification

All fixtures were created in a new disposable Debian 13 guest, CT 9422
(`aimee-repair-042-20260906`). Production CT 103 was not modified. The earlier validation guest
with the same numeric ID was removed before this guest was created.

| Gate | Result |
| --- | --- |
| C registry | Complete parallel suites pass with SQLite and real PostgreSQL; 637 registered executables per backend, plus the suite's auxiliary checks. |
| Repository lint | All 77 checks pass, including formatting, module ownership, source boundaries, generated API parity, and schema checks. |
| Go | Complete server-go suite and runtime-web suite pass. Memory privacy, scope ordering, wire verdicts, and retrieval corpus pass with real PostgreSQL and the race detector. |
| Frontend | 198 tests pass; runtime and console production builds pass. |
| Fresh split stack | Standard 10-probe bootstrap smoke passes; [61 memory checks](release-0.4.2-repairs-2026-09-06/memory-fresh-complete.json) pass with web enabled. |
| 0.4.1 upgrade stack | [67 memory checks](release-0.4.2-repairs-2026-09-06/memory-upgrade-complete.json) pass over preserved 0.4.1 volumes, including the old global/project records and web-disabled error handling. |
| Published thin client | [Six compatibility checks](release-0.4.2-repairs-2026-09-06/published-client-final.json) pass using the previously published binary against the repaired server's argument specifications. |
| Browser | [Five checks](release-0.4.2-repairs-2026-09-06/browser-memory.json) pass using real PAM authentication, the built GUI, and real services: scope switching, local retirement, colliding KB preservation, and desktop/mobile rendering without JavaScript errors. |
| Learning loops | 46 checks pass through the corrected disposable-database wrapper. |
| Module liveness | 17 checks pass in a fresh database, including real capture failure, durable audit evidence, attached modules, learning capture, and no unexpected blockers. |
| Upgrade policy | Entry-point tests cover untouched old grants, idempotence, and preservation of operator changes. |

The browser fixtures use synthetic personal data. Screenshots show the
[personal view](release-0.4.2-repairs-2026-09-06/personal.png) and
[mobile KB view](release-0.4.2-repairs-2026-09-06/kb-mobile.png).

## Reproduction and artifact boundary

`tests/e2e/memory-placement-e2e.py` deliberately writes fixtures and interrupts dependencies;
run it only against explicitly named disposable containers. The split Docker CI topology
invokes it through `AIMEE_E2E_MEMORY_PLACEMENT=1`. The existing PostgreSQL memory CI gate now
requires the privacy and clean-verdict regressions alongside the retrieval corpus. New C
regressions are registered in the ordinary unit-test suite.

The repair images are local overlays on the published testing images. They contain rebuilt
server/CLI binaries using the published server's Bookworm ABI, the rebuilt KB, memory module,
runtime-web service and frontend, and the repaired KB entrypoint. Image and artifact hashes
are retained in [repair-summary.json](release-0.4.2-repairs-2026-09-06/repair-summary.json).
These are verification artifacts, not signed/published 0.4.2 release artifacts. The protected
release workflow still owns the final versioned builds, signatures, tags, and publication.

The original broader exploratory and provider acceptance results remain recorded separately.
No live external model-vendor subscription was exercised. A passing page-render or fixture
suite is not a claim that every optional integration or deployment architecture was exercised.
