# workspace module

## Purpose and non-goals

`workspace` is required core and owns scoped resource identity, path containment, provider binding,
working-tree lifecycle, per-turn filesystem/process context, and local coding-agent resource access.
It does not own Git repository semantics, tool schemas, action authorization, credentials, memory
scope semantics, or optional workflow orchestration.

### Go process stage

The supervised `workspace-access` stage now runs in the shared pure-Go module
runtime. It preserves the WREF/WWOK contract and validates bounded
`owner/repository` references before admitting access. The C adapter remains a
wire-parity fixture. The C scope seam fails closed until the server registers
its event-bus provider and has no local reference-validation fallback. Provider
binding, containment, runner queues, mirrors, manifests, and worktree lifecycle
remain in C for later isolated migrations.

## Public contracts

`src/modules/workspace` owns `workspace_active_root` at `src/modules/workspace/workspace.c:117`,
worktree creation at line 1175, manifests and handles,
`workspace_provider_t`, per-turn binding, detached/container providers, mirrors, runner queues, and
`ws_scope_*` containment. The duplicated worktree declarations in `guardrails.h` are a
compatibility/ownership seam to consolidate, not a second workspace implementation.

The descriptor declares this module's eleven sources, eleven module-root headers, eleven direct
tests, and this document; it sets `ownership_complete: true`. All eleven headers are declared as
`private_headers` because they live at the module root rather than under
`src/modules/workspace/include/aimee/workspace/`, the layout the header-layout checker treats as
private; `workspace_provider.h` is the provider dispatch interface and has no paired source, while
`cli_workspace_serve.c` has no paired header. Three sources, `workspace_provider_container.c`,
`workspace_provider_detached.c`, and `workspace_runner_queue.c`, have no external includer but are
live module-internal units: the container and detached providers are selected through
`workspace_provider.h` by `workspace_turn.c` and `cli_workspace_serve.c`, and the runner queue is
consumed through `workspace_runner_registry.h`. Make compiles all eleven sources; CMake compiles the
four the thin `aimee` client reaches (`cli_workspace_serve.c`, `workspace.c`, `workspace_manifest.c`,
`workspace_provider_detached.c`) and omits the seven server/runner-side units, the same intentional
thin-client boundary recorded for gateway and learning. `docs/validation/core-modularization-slice-44.md`
records the declaration audit and `docs/validation/core-modularization-slice-45.md` the completeness
audit; the two were split so the latch reviews declarations merged on their own first. Adding a new
module-local source or module-root header without declaring it now fails CI on `rule=ownership-complete`.

## Dependencies and consumers

- `audit`: carries the runner questions to the module over the bus, which is how this module asks
  who is serving a tree and hands work to that client.
- `config`: supplies workspace registrations, provider metadata, mirrors, and sandbox overrides.
- `execution-policy`: authorizes filesystem, process, network, and lifecycle effects within a workspace.
- `module-runtime`: supplies required lifecycle and readiness contracts for resource access.
- `vault`: supplies attested principal identity and scoped credentials used by remote resource paths.

Consumers include [delegates](delegates.md), `tools`, `git`, [memory](memory.md), server sessions,
and optional workflows. A consumer receives a scoped handle/provider; it must not infer authority
from an arbitrary current directory or replace a requested isolated provider with host execution.

## Providers and readiness

The shared local `workspace_provider_shared` provider is the required reference implementation; detached, mirror, and container
paths bind through their specialized lifecycle contracts. Readiness requires one provider appropriate
to the selected workspace and supported operation. A missing detached runner or container handle must
fail closed; generic kind resolution must not silently defeat an explicit isolation requirement.

## Configuration and activation

- `runtime_toggle.supported`: `false`; scoped resource access is required while individual workspaces and providers are configurable.

### Config touchpoint

The module consumes `workspaces[]`, provider, VCS remote/head, sandbox image, and workspace-root
fields declared at `src/modules/config/config.h:270` and parsed at `src/modules/config/config.c:1572`;
`config` owns parsing and persistence. Workspace interprets registrations into bounded
handles. Fields for detached, mirror, or container behavior are truthful only when that live provider
path is registered and usable.

## Surfaces

Surfaces include `aimee workspace add|list|remove|serve`, `/v1/workspaces` handles and runner polling,
worktree/session isolation, manifests, mirror drift notices, and provider diagnostics. File and process
tools use the active provider but remain `tools` surfaces; Git status, commits, refs, and forge actions
remain `git` surfaces.

## Data and migrations

Workspace state includes configured registrations, provider/VCS metadata, `workspace_manifest` records, runner queues,
mirror state, session-to-worktree mappings, and transient provider bindings. Migration must preserve
stable workspace identity, principal scope, provider kind, root containment, VCS head, and cleanup
ownership; queue payloads and live container handles are ephemeral rather than durable authority.

## Security and privacy

`ws_scope_*` validates principal/project components and uses openat-family beneath/no-symlink opens,
including `O_NOFOLLOW`, where required.
Repository content, manifests, tool output, runner responses, subprocess argv/environment, and config
are untrusted. Provider binding and policy checks precede raw I/O; paths, secrets, and private file
contents must be redacted from diagnostics, and isolation requests cannot degrade to shared host access.

## Supported journeys

A principal selects a registered workspace; `workspace_turn_bind_active` validates its root and binds
the correct provider; execution-policy authorizes each operation; tools or delegates perform bounded
I/O; Git may mutate repository state inside the same root; and turn teardown clears bindings and
reconciles or cleans transient worktrees according to explicit ownership and dirty-state rules.

### Working-tree boundary

Workspace creates, names, reuses, locks through its lifecycle, and removes Aimee worktrees; `git` owns
index, refs, commits, ignore interpretation, and repository mutations inside them. Workspace owns
transient mapping/runner state, while Git owns tracked state. Concurrent access is mediated by distinct
session/delegate worktrees and registries; locking outside Git/worktree primitives is a hypothesis, unverified.

## Tests and failure behavior

The descriptor's eleven direct tests are `test_workspace.c`, `test_workspace_handle.c`,
`test_workspace_manifest.c`, `test_workspace_mirror.c`, `test_workspace_provider.c`,
`test_workspace_provider_container.c`, `test_workspace_provider_detached.c`,
`test_workspace_runner_queue.c`, `test_workspace_runner_registry.c`, `test_workspace_scope.c`, and
`test_workspace_turn.c`, covering the implementation. `test_workspace_memory.c` carries the workspace
name and links `workspace.o` but is a memory test: its subject `memory_auto_tag_workspace` is defined
in `src/modules/memory/memory_core.c`, so it exercises memory's workspace-scoped tagging and is not
claimed here, the same way learning does not claim the KB `test_learning_synth.c`. Invalid or foreign
roots, traversal/symlink escape, missing runner, drift, provider mismatch, and dirty cleanup must
surface explicitly. A failed isolated binding must never retry through the shared host provider.

## Operational diagnostics

Report `workspace_id`, safe root identity, principal class, provider kind, binding/runner state, mirror
head and drift category, worktree/session identity, dirty status, and cleanup result. Diagnostics must
distinguish unregistered, unauthorized, unreachable, divergent, and provider-unavailable states while
redacting credentials, private file bytes, and runner request environments.
Cross-module working-tree evidence is collected in the [Slice 16 validation record](../validation/core-modularization-slice-16.md).

## Compatibility

Workspace IDs, registration shape, `workspace_provider_t`, handle manifests, per-turn binding, path
containment, worktree naming, and cleanup behavior are compatibility contracts. Moving duplicate
guardrail helpers or platform shims must preserve call semantics and avoid parallel registries; provider
aliases cannot silently change an isolated workspace into a shared one.

## Extension and removal

New resource providers implement the narrow `workspace_provider_t` raw-I/O interface; they make no
policy decision because authorization and path validation remain above it. Mirror, detached, and container implementations must each prove a live acquisition-to-release
journey; registry-only or self-test-only paths are dead-code candidates. Workspace cannot be optional
because every delegate/tool execution requires bounded resource authority, including non-Git directories.
