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
| Agent roster, sealed credential storage/resolution, delegate execution, and roundtable runs | C resource plane, consumed by Go |
| Non-WFE chat, memory, workspace, and administrative APIs | Existing service owners |

The C HTTP dispatcher returns `410 Gone` for every WFE lifecycle endpoint even
if it is reached directly. The C process does not register or start its former
workflow executor or scheduler. Its remaining workflow-named source files are
retirement debt only; they are not a supported runtime path and can be deleted
incrementally as their non-lifecycle helpers are moved behind the Go resource
client.

The local-only `/v1/internal/forge/execute` resource route is the credential
boundary between those planes. It accepts only a small typed set of mechanical
forge operations from a kernel-attested Unix-socket peer. C confines the path to
the managed WFE worktree root, derives repository identity from the worktree's
Git common directory, verifies branch shape,
and applies the shared vault resolution ladder without returning credential
material. It does not read DB1, choose an operation, interpret a workflow, or
advance a transition. Go owns those decisions and every resulting lifecycle
transition.

The image uses `tini` as PID 1. The shell entrypoint supervises the C resource
plane and Go WFE plane as peers: exit of either terminates the other and exits
the container nonzero, while TERM/INT invokes bounded shutdown of both.
