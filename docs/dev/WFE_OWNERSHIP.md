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

The C HTTP dispatcher proxies supported WFE requests to the Go Unix socket. Retired lifecycle
endpoints return `410 Gone` if reached directly. The C process does not start its former workflow
executor or scheduler. Its remaining workflow-named source files are retirement debt, not a supported
runtime path.

The local-only `/v1/internal/forge/execute` route is the credential boundary between those planes. It
accepts a small set of typed forge operations from a kernel-attested Unix-socket peer. C confines the
path to the managed WFE worktree root, derives repository identity from Git, and requires the checked
out branch to match the work-item ID and managed feature or slice namespace.

Push uses an explicit HTTPS destination and refspec under a minimal environment. A final feature PR
targets the branch checked out when Go admitted the repository. A slice targets the exact parent
feature branch derived from its Go-generated `<root>.s<10hex>.g<generation>.<index>` ID. The request
schema rejects unknown fields, duplicate fields, invalid operation combinations, and caller-supplied
repository identity.

C resolves credentials without returning them. It does not read workflow state, choose an operation,
interpret a graph, or advance a transition. Go owns those decisions and their lifecycle evidence.

The Go ID generator is `native_runner.go`'s foreach fan-out. At the mechanical
boundary, `wfe_item_id_valid` mirrors that grammar and
`wfe_slice_ref_matches_workdir` derives the only valid slice head and parent
feature target.

The image uses `tini` as PID 1. The shell entrypoint supervises the C resource
plane and Go WFE plane as peers: exit of either terminates the other and exits
the container nonzero, while TERM/INT invokes bounded shutdown of both.
