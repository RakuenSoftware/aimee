# git module

## Purpose and non-goals

`git` is required core and owns repository state/history access, repository mutation, verification,
forge operations, and provenance-bearing repository records submitted through memory's public ingest
boundary. It does not own workspace path authority, code-intelligence storage/indexing/retrieval,
federated OIDC governance, generic tool dispatch, or secret custody.

## Public contracts

`src/modules/git` owns `git_ops`, MCP `handle_git_*` operations beginning at
`src/modules/git/mcp_git_query.c:321` and `src/modules/git/mcp_git_write.c:53`, verification, host/remote resolution,
projects, forge credentials/API, OAuth device flows, and SSH-agent setup. The approved target contract
is `memory.repository-record.ingest.v1`: Git is submit-only, while [memory](memory.md) retains schema,
redaction acceptance, persistence, embedding, reranking, and code intelligence.

## Dependencies and consumers

- `audit`: records repository reads, mutations, provenance, policy decisions, and outcomes.
- `config`: supplies verification, forge, credential, identity, and repository-operation settings.
- `execution-policy`: authorizes repository, network, credential, hook, and subprocess effects.
- `memory`: exclusively owns code-intelligence persistence and accepts redacted repository records.
- `module-runtime`: supplies required lifecycle and readiness contracts for repository capability.
- `vault`: supplies principal-scoped forge tokens, SSH keys, and credential references.
- `workspace`: supplies the authorized root, worktree lifecycle, and process/filesystem provider.

Consumers include `tools`, [delegates](delegates.md), [protocols](protocols.md), memory ingest, and
optional workflows. A non-Git workspace remains usable; Git-only requests return typed
`capability_absent` without disabling the workspace or core runtime.

## Providers and readiness

Local `git` CLI operations are the required reference path; forge HTTP, OAuth, SSH, and organization/PR
helpers are provider-specific capabilities beneath the module. Core readiness does not require a forge
account, but repository operations must report whether local Git, a repository, credentials, signing,
or a requested forge capability is absent instead of claiming a generic ready state.

## Configuration and activation

- `runtime_toggle.supported`: `false`; repository capability is core while repositories, forges, credentials, and verification policy are configurable.

### Config touchpoint

The module consumes project verification config, Git identity, forge/provider credentials, live-forge
gates registered at `src/modules/config/config_fields.c:146`,
gates, and shell-Git restrictions; `config` owns general parsing and projection. OAuth-for-forge settings
belong to Git providers. Federated OIDC/SSO settings belong to optional governance and must not appear as
a required Git configuration dependency.

## Surfaces

Surfaces include native/MCP `git_status`, diff, log, branch, commit, push, pull, fetch, clone, restore,
reset, stash, tag, issue, PR, and `aimee git verify` operations plus project/forge credential flows.
Protocol and tools modules expose these operations, but Git owns their repository semantics, credential
containment, verification gates, and mutation results.

## Data and migrations

Git state includes repository index/refs/commits, verify configuration/state, `git_project` mappings, host
credential references, OAuth device state, PR/CI results, and signed provenance. Repository-derived
records are assembled, secret-redacted, principal-scoped, and submitted to memory; Git may not write a
memory-owned code-intelligence namespace or persist partial pre-redaction records.

## Security and privacy

Repositories, hooks, configs, submodules, forge responses, tool output, argv/environment, and config are
untrusted. Policy gates mutations and network use; workspace constrains paths; vault retains custody.
`git_cred_inject_build_env` at `src/modules/git/git_cred_inject.c:235` and SSH/askpass seams handle raw values and therefore must bound inheritance,
avoid argv/logging, strip child environments, and fail closed when principal or scope is missing.

## Supported journeys

Tools or a delegate request a repository operation; workspace resolves the authorized root;
execution-policy approves the typed effect; vault supplies a scoped credential only when needed; Git
performs the local or forge operation and returns a bounded result with audit evidence. Repository
ingest additionally requires distinct producer/repository provenance and complete pre-ingest redaction.

### Working-tree boundary

`workspace` creates/removes Aimee worktrees, owns their path layout and transient mappings, and isolates
concurrent sessions. Git owns `.git`-adjacent index, refs, ignore behavior, commits, and repository
mutations within the selected root. Git must not delete a workspace to recover from repository failure;
locking outside Git/worktree primitives is a hypothesis, unverified.

## Tests and failure behavior

`test_mcp_git.c`, git ops/project/host/credential/OAuth/SSH/PR/verify suites, guardrail Git tests, and
integration tool calls cover current behavior. Missing repository, denied mutation, dirty/conflicting
state, invalid ref/path, failed signature/redaction, absent credential, forge error, or failed verify step
must return typed failure; non-Git base workspace operations continue normally.

## Operational diagnostics

Report safe repository identity, workspace/principal, `git` operation, branch/ref, dirty state, verification
step, credential reference/scope, forge host/provider, PR/CI state, provenance verification, redaction,
and bounded stderr. Diagnostics must exclude tokens, SSH private keys, credential environments, private
source bytes, and complete forge response bodies unless explicitly redacted.
Cross-module Git/workspace evidence is collected in the [Slice 16 validation record](../validation/core-modularization-slice-16.md).

## Compatibility

Git tool names and schemas, verify contracts, repository result shapes, project/host resolution,
credential injection, PR/CI grading, non-Git `capability_absent`, and memory-ingest invariants are
compatibility contracts. Provider aliases may translate forge APIs but cannot make GitHub-specific
OAuth equivalent to generic OIDC governance or bypass the canonical repository boundary.

## Extension and removal

New forge or credential providers implement bounded Git seams without expanding the module taxonomy.
Provider-specific OAuth remains distinct from optional governance OIDC. Shell wrappers, duplicate tool
schemas, and forge helpers with no caller beyond registration/tests are `configuration-only`, `test-only`,
or `duplicated-by-adjacent-module` candidates; deletion requires later runtime liveness evidence.
