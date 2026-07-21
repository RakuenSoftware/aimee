# WFE runtime ownership

WFE has one runtime owner: `aimee-wfe`, the Go control plane. The server image
does not support switching lifecycle execution back to C. `AIMEE_WFE_ENGINE`
defaults to `go` and any other value fails startup.

| Capability | Runtime owner |
| --- | --- |
| Workflow definitions, custom blocks, validation, and immutable snapshots | Go |
| Trigger scanning, deduplication, and admission | Go |
| Work-item lifecycle, scheduling, parks, retries, budgets, and fan-out | Go |
| Artifact persistence, worktrees, verification, forge operations, and final PR | Go |
| Workflow GUI API (`/v1/workflow/*`, `/v1/trigger/fire`, `/v1/dev/submit`) | Go socket via webchat |
| Agent roster, credentials, delegate execution, and roundtable runs | C resource plane, consumed by Go |
| Non-WFE chat, memory, workspace, and administrative APIs | Existing service owners |

The C HTTP dispatcher returns `410 Gone` for every WFE lifecycle endpoint even
if it is reached directly. The C process does not register or start its former
workflow executor or scheduler. Its remaining workflow-named source files are
retirement debt only; they are not a supported runtime path and can be deleted
incrementally as their non-lifecycle helpers are moved behind the Go resource
client.

The image uses `tini` as PID 1. The shell entrypoint supervises the C resource
plane and Go WFE plane as peers: exit of either terminates the other and exits
the container nonzero, while TERM/INT invokes bounded shutdown of both.
