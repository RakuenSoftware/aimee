# Local recall through the existing memory module

Status: this integration is tested; the expanded KB-optional release remains incomplete.
Memory was already a module in both placements. Its local recall path incorrectly depended
on a KB-produced bundle. Recall now runs through the same Go data stage using the authorized
user store, with no shared table or KB identity required for personal record operations.

API, CLI, and MCP recall default to user memory. Explicit KB recall excludes personal rows.
The compatibility context consumer retains its combined recall when KB is configured and
uses local module recall when KB is disabled or unreachable. Session-start API recall needs
no task hint. Scoped handles and prompt text survive the module/transport boundary.

Verification used disposable guest 9422 on 192.168.1.253; production guests were untouched.
The local stack started from empty volumes and contained only Server and PostgreSQL. The
verification images overlay locally rebuilt binaries on the testing base; they are not
published release images.

| Check | Result |
| --- | --- |
| Fresh KB-free personal storage/recall through API, CLI, MCP; restart; local DB outage/recovery; retirement | 15/15 |
| Combined Server + KB placement, colliding IDs, explicit shared recall, local recall during KB outage, all-table personal canary scan | 70/70 |
| Standalone health/version/auth/missing-KB smoke | 4/4; fixed its false failure exit when retaining the stack |
| Go memory module with PostgreSQL and race detector | Pass; shared table absent, retired/expired rows excluded |
| Served CLI argument contracts | 127 specs, 1,247 differential samples pass |
| MCP schema/client registry | Pass |
| Native agent suite with PostgreSQL | Pass using the standard config/provider fixtures and disposable DB grants |
| Frozen semantic-context integration checks | 14 tests pass; only exact reviewed memory changes allowed |
| Memory C boundary | Pass |
| Required repository lint | 77/77 |

The persistent deployment checks are in `tests/e2e/memory-placement-e2e.py`. Omitting `--kb`
selects the local test. Existing CI memory-placement activation now runs this in T3 as well
as the combined checks in T2.

Evidence: [local checks](release-0.4.2-local-recall-2026-09-06/memory-local-recall.json) and
[combined checks](release-0.4.2-local-recall-2026-09-06/memory-recall-shared.json).

Remaining release work includes personal vector persistence, embedding/synthesis service
provisioning and credentials owned by either composition, setup/readiness with KB optional,
and full qualification of those changes. Structured reminders and directives still require
the shared schema. Recall's existing approximate token packing also needs qualification for
large records. The previous hosted CI run 34059119430 passed sanitizers and all Docker
stacks but failed three workflow scheduler tests; those failures remain unresolved.

An additional direct descriptor-validator invocation reports an existing ownership-boundary
error for `server-go/cmd/aimee-memory-bus-probe/main.go` in the memory manifest. That source
entry is unchanged by this recall repair and still needs review during module composition.
