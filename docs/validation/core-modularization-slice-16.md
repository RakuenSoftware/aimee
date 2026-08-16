# Core modularization slice 16: execution-authority documentation

## Diff scope precondition

The slice-start commit is `610851a04f08ce210c17a65c7b1c98d2ad424326`. The allowed close diff is
limited to these five module documents, the closed documentation-status partition, this validation
record, and the existing cleanup ledger:

- `docs/modules/execution-policy.md`
- `docs/modules/vault.md`
- `docs/modules/workspace.md`
- `docs/modules/tools.md`
- `docs/modules/git.md`
- `tests/baselines/modules/documentation-status.yaml`
- `docs/validation/core-modularization-slice-16.md`
- `tests/baselines/refactor/cleanup-ledger.json`

Close-time `git diff --cached --numstat` includes eight files, 747 insertions, and
7 deletions. All seven deletions are in the status partition: five
promoted IDs plus two surviving list lines reserialized to add or remove a trailing comma.

No production source, descriptor, build graph, route, configuration behavior, database, GUI, checker,
or runtime profile changes are allowed. The close-time `git diff --name-only` and `git diff --stat`
checks enforce this path set; no exception channel is used.

## Outcome

This slice promotes exactly `execution-policy`, `vault`, `workspace`, `tools`, and `git` from
documentation debt. They form the required execution-authority family: policy authorizes effects,
vault mediates secret custody, workspace supplies bounded resource authority, tools exposes and
dispatches typed capabilities, and Git owns repository semantics. This is a documentation/baseline
classification, not a claim that physical consolidation or the future executable core proof is complete.

The eight remaining documentation-debt entries are `audit`, `benchmarks`, `config`, `control-web`,
`governance`, `roundtable`, `runtime-web`, and `workflows`.
Within this closed debt set, `audit` is required-core deferred; the other seven entries are optional.

## Slice 16 Reconciled Appendix

### Method and limits

The inspection used descriptor dependencies, definitions/references, CLI/API surfaces, configuration
readers, test targets, and the already approved Git/core contracts. Each module document retains the
13-section schema enforced by `scripts/check_module_docs.py`; requested ownership, effect, credential,
workspace, and config-touchpoint evidence is embedded in those canonical sections rather than creating
a competing template.

The review was static. It can identify definitions, compiled tests, registrations, and visible callers,
but it cannot prove runtime reachability, absence of dynamically constructed calls, secret erasure on
every failure path, or cross-process locking. Such claims are labeled **hypothesis, unverified**. A
dead-code **candidate, not confirmed** requires no static import/reference and no configuration or
registration reference in the inspected surface. Candidate vocabulary is restricted to `unreachable`,
`superseded`, `configuration-only`, `test-only`, and `duplicated-by-adjacent-module`.

The threat model covers untrusted repository files, hooks, configs and submodule pointers; untrusted
tool output and structured results; untrusted subprocess argv/environment; and untrusted configuration.
Network-position attackers, dependency supply-chain compromise, side channels, and physical access are
outside this slice's static boundary claims.

Deferred modules were inspected only at their boundary touchpoints: `audit` action calls, workflow native
gates/forge calls, governance dependencies, and config readers. Their internal architectures and final
ownership are not asserted here. The GUI modules, benchmarks, and roundtable required no deeper inspection.

### Inspected inventory and evidence

### Execution policy

Owned/target evidence includes `src/modules/execution-policy/module.yaml`; the distributed
`src/modules/guardrails/*`; `src/server/agent_policy.c`; `src/server/agent_policy_intercept.c` and its
header; and `src/gateway_policy.c` and its header. Key entry points are
`pre_tool_check` (`src/modules/guardrails/guardrails_action_audit.c:136`), `pre_tool_check_inner`
(`src/modules/guardrails/guardrails_orchestrator.c:1197`), `tool_validate`
(`src/server/agent_policy.c:252`), `policy_load` (`src/server/agent_policy.c:395`), and
`gateway_policy_apply_request` (`src/gateway_policy.c:94`). Tests inspected include guardrails,
gateway-policy, agent-policy-intercept, tool-validation, and workflow native-gate suites.

### Vault

Owned evidence includes all `src/modules/vault/*`; storage provider `src/modules/db2/c/vault_pg.*`; server entry
points in `src/server/server_vault*.c`; and KB rewrap/binding touchpoints. The service boundary begins at
`vault_service_unlock` (`src/modules/vault/vault_service.c:150`), set/get at lines 265/294, injection at
line 360, principal resolution at `src/modules/vault/vault_principal.c:31`, storage binding at
`src/modules/vault/vault_store.c:995`, and server-key readiness at
`src/modules/vault/vault_server_key.c:461`. Vault service/store/seam, audit, capability, custody,
principal, seal, rotation, bootstrap, and PostgreSQL tests were inspected.

Static raw-secret consumers outside vault are: provider credential buffers in
`src/server/agent_config.c:334`; delegate retry/injection in
`src/modules/delegates/delegate_credential_retry.c:39`; forge token/SSH-key buffers in
`src/modules/git/git_forge_vault.c:15`; Git credential resolution at
`src/modules/git/git_cred_inject.c:166`; Git SSH-agent key loading in
`src/modules/git/git_ssh_agent.c:134`; server OAuth client material in `src/server/oauth_tokens.c:57`;
and PKI key material in `src/server/pki.c:554`. The repository-scoped environment builder starts at
line 177, its compatibility wrapper at line 235, and cleanup zeroes the token environment through
`git_cred_inject_free_env` at line 265. Complete dynamic non-logging and non-persistence across every consumer is
a **hypothesis, unverified**. Log/persistence inspection covered those consumers, `vault_service.c`,
`vault_store.c`, server vault handlers, and delegate credential retry; no runtime trace was collected.

### Workspace

Owned evidence includes all `src/modules/workspace/*`, platform provider implementations under
`src/posix` and `src/windows`, CLI workspace dispatch in `src/cli_main.c`, server workspace/runner routes,
and config registration readers. Key boundaries are `workspace_active_root`
(`src/modules/workspace/workspace.c:117`), worktree creation (line 1175),
`workspace_turn_bind_active` (`src/modules/workspace/workspace_turn.c:249`), runner registry creation
(`src/modules/workspace/workspace_runner_registry.c:31`), and TOCTOU-safe project open
(`src/modules/workspace/workspace_scope.c:343`). Workspace, handle, manifest, mirror, provider,
detached/container, runner, scope, and turn tests were inspected.

### Tools

The target `src/modules/tools` directory currently contains only its descriptor. Distributed owned
candidates include `src/headers/agent_tools.h`, `src/server/agent_tools.c`, platform `agent_tools*`,
`src/toolset.c`, `src/headers/toolset.h`, `src/modules/db2/c/tool_registry.*`, tool argument/schema helpers,
`src/cmd_toolset.c`, and `src/tool_prompts/*`. The live seams are `build_tools_array`
(`src/server/agent_tools.c:1492`), Git-provider registration (line 1344),
`dispatch_tool_call_ctx` (`src/posix/agent_tools_dispatch.c:1884`), effective toolsets
(`src/toolset.c:534`), and DB2 lookup (`src/modules/db2/c/tool_registry.c:10`). Toolset, validation, schema,
arguments, output, prompts, MCP native dispatch/surface, script-runner, and server-compute tests were inspected.

### Git

Owned evidence includes all `src/modules/git/*`, Git route wiring in
`src/server/server_http_routes_git.c`, the approved `git-core-contract.md`, and native/MCP/CLI consumers.
Key effects are status (`src/modules/git/mcp_git_query.c:321`), commit/push
(`src/modules/git/mcp_git_write.c:53` and `:211`), verification
(`src/modules/git/git_verify.c:1409`), credential environment construction
(`src/modules/git/git_cred_inject.c:177`), and project remote resolution
(`src/modules/git/git_project.c:1305`). MCP Git, ops, project, credential, OAuth, SSH, PR/CI, verify,
guardrail, and webchat leak tests were inspected.

### Authority-flow matrix

| Verified operation | Authorizes | Exposes capability | Supplies credentials | Selects workspace | Mutates repository state |
|---|---|---|---|---|---|
| Native file/process tool call | `execution-policy` | `tools` via protocols/delegates | `vault` when explicitly required | `workspace` | no Git metadata mutation; workspace may change working-tree bytes |
| Git commit or push tool | `execution-policy` | `tools`/protocols | `vault` through Git injection | `workspace` | `git` owns index, refs, commit, and remote mutation |
| Delegate provider invocation | `execution-policy` | [delegates](../modules/delegates.md) | `vault` | `workspace` | none unless the delegate invokes a Git capability |
| Vault set, unlock, or rotate | `execution-policy` | `vault` CLI/API | `vault` owns custody | no workspace required | none |
| Detached workspace file operation | `execution-policy` | `tools` or Slice-15 `delegates` | `vault` supplies principal/remote material where required | `workspace` | no index/ref mutation unless routed to `git` |
| Repository-record ingest | `execution-policy` | `git` submits to [memory](../modules/memory.md) | `vault` supplies scoped signing/transport material | `workspace` | `git` reads state; `memory` alone persists code intelligence |

This matrix describes verified ownership, not a guaranteed runtime call order. Descriptor dependencies
remain authoritative and do not form the narrative chain as a simple topological sequence.

### Working-tree boundary

`workspace` selects roots, validates containment, creates/reuses/cleans Aimee worktrees, binds providers,
and owns transient session/runner mappings. `git` owns repository index, refs, commits, ignore semantics,
remote operations, and verification of Git state inside that root. File tools may change working-tree
bytes through workspace authority but do not thereby own Git metadata. Concurrent delegates receive
separate session/work-name worktrees; Git and worktree primitives mediate known collisions. Locking
outside those primitives is a **hypothesis, unverified**.

### Overlap and liveness findings

| Pair | Evidence | Class | Disposition |
|---|---|---|---|
| `execution-policy::pre_tool_check` ↔ `tools::dispatch_tool_call_ctx` | `guardrails_action_audit.c:136`; `agent_tools_dispatch.c:1884` | boundary handoff, not duplicate | `retain`; relocate only in a source slice |
| `execution-policy::policy_is_source_discovery` ↔ [memory code intelligence](../modules/memory.md) | `src/server/agent_policy_intercept.c:113`; `src/cli_main.c:1986` | policy/command ownership boundary, not duplicate | `relocate` the classifier only in an execution-policy source slice |
| `execution-policy::gateway_policy_apply_request` ↔ [gateway policy](../modules/gateway.md) | `src/gateway_policy.c:94`; `src/server/anthropic_http.c:345` | overlap with a documented execution module | `defer` to an execution-policy source slice |
| `tools::agent_tools Git schemas/providers` ↔ `git::mcp_git handlers` | `src/server/agent_tools.c:807`; `src/modules/git/mcp_git_query.c:321` | `duplicated-by-adjacent-module` candidate, not confirmed | `defer` schema/dispatch consolidation |
| `workspace::worktree_* declarations` ↔ `execution-policy::guardrails.h worktree declarations` | `src/modules/workspace/workspace.h:67`; `src/modules/guardrails/guardrails.h:219` | `duplicated-by-adjacent-module` candidate, not confirmed | `relocate` in a workspace source slice after comparison |
| `tools::tool_git_* platform readers` ↔ `git::mcp_git_query` | `src/posix/agent_tools.c:1817`; `src/modules/git/mcp_git_query.c:321` | `duplicated-by-adjacent-module` candidate, not confirmed | `defer` behavior/caller comparison before deletion |
| `vault::vault_store_backend_t` ↔ `db2::vault_pg_backend` | `src/modules/vault/vault_internal.h:27`; `src/modules/db2/c/vault_pg.c:745` | provider implementation, not duplicate authority | `retain` behind the vault facade |

No `unreachable`, `superseded`, `configuration-only`, or `test-only` candidate met the static threshold.
Each substantial provider family has a non-test definition/reference, route, registry, or configuration
touchpoint. That is not runtime liveness proof; dynamic reachability remains for later source slices.

### Deferred touchpoints

- `config`: parsing/projection stays deferred; each module doc includes one `Config touchpoint` subsection.
- `audit`: required core event ownership will be documented later; this slice records it only as a dependency.
- `workflows`: native gates and live-forge calls are consumers, not execution-policy or Git owners here.
- `governance`: owns federated OIDC/SSO and organizational policy; local enforcement and custody stay core.
- `benchmarks`, `roundtable`, `control-web`, `runtime-web`: no ownership decision is made in this slice.

## Verification

- `python3 -I -S scripts/check_module_docs.py`
- `python3 -I scripts/tests/test_check_module_docs.py -v`
- `python3 -I -S scripts/check_module_source_ownership.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I -S scripts/refactor_baselines.py`
- `make -C src lint`
- close-time changed-path and diff-stat scope checks
- feature-branch pull-request CI
