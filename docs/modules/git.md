# git module

## Purpose and non-goals

`git` is required core and owns repository state/history access, repository mutation, verification,
forge operations, and provenance-bearing repository records submitted through memory's public ingest
boundary. It does not own workspace path authority, code-intelligence storage/indexing/retrieval,
federated OIDC governance, generic tool dispatch, or secret custody.

### Go process stages

The supervised `git-operation` and `git-ref-validation` stages run in the shared
pure-Go module runtime. The first preserves the GOPS/GCLS contract, classifies
the bounded operation, and marks operations that require credentials. The
second owns the conservative branch/ref allowlist used before checkout and
managed push. Both production decisions cross the local event bus and fail
closed when the process is unavailable or returns malformed data; there is no
server-local ref-policy fallback. The C adapter remains a wire-parity fixture.
Repository execution, verification, OAuth, forge, SSH, and credential
implementation units remain C relocation work.

## Public contracts

`src/modules/git` owns `git_ops`, MCP `handle_git_*` operations beginning at
`src/modules/git/mcp_git_query.c:321` and `src/modules/git/mcp_git_write.c:53`, verification, host/remote resolution,
projects, forge credentials/API, OAuth device flows, and SSH-agent setup. The approved target contract
is `memory.repository-record.ingest.v1`: Git is submit-only, while [memory](memory.md) retains schema,
redaction acceptance, persistence, embedding, reranking, and code intelligence.

The descriptor declares this module's twenty-seven sources, eighteen module-root headers, fourteen
direct tests, and this document; it sets `ownership_complete: true`. All eighteen headers are
declared as `private_headers` because they live at the module root rather than under
`src/modules/git/include/aimee/git/`, the layout the header-layout checker treats as private; sixteen
pair with a source, and two have no paired source: `git_verify_internal.h` (the verify-family seam)
and `mcp_git.h` (the shared MCP-git header). Make compiles all twenty-seven sources; CMake compiles the
thirteen the thin `aimee` client reaches (the eight `git_verify_*` sources and the five `mcp_git_*`
tools) and omits the fourteen credential, OAuth, ops, forge-vault, host, org-repos, and PR-API sources
that are server/kb-side, the same intentional thin-client boundary recorded for gateway, learning,
workspace, vault, and config. This descriptor's `ownership_complete` latch is independent of the
separate `git-core-contract` governance, which bounds git's core capability rather than its file
ownership. `docs/validation/core-modularization-slice-50.md` records the declaration audit and
`docs/validation/core-modularization-slice-51.md` the completeness audit; the two were split so the
latch reviews declarations merged on their own first. Adding a new module-local source or module-root
header without declaring it now fails CI on `rule=ownership-complete`.

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

Surfaces include native/MCP `git_status`, diff, log, branch, add, commit, push, pull, fetch, clone,
restore, reset, stash, tag, issue, PR, merge, rebase, sync, cherry-pick, revert, switch, checkout and
verify operations plus project/forge credential flows. Protocol and tools modules expose these operations,
but Git owns their repository semantics, credential containment, verification gates, and mutation results.

The `aimee` CLI reaches all of them through one wildcard `/v1` route (`{"git", NULL, "git.cli", ...}` in
`cli_v1_routes.c`, marshalled by `marshal_git_cli`) that dispatches `mcp.call` with `tool=git_<command>`.
Previously only `git verify` was routed and every other `aimee git ...` answered "is not a subcommand of
'git'". The CLI grammar is uniform rather than per-command: `aimee git <command> [primary] [key=value ...]`,
where the table in `marshal_git_cli` holds the only per-command knowledge (what a bare first, and
sometimes second, word means). Values are typed the way verify's already are, `continue`/`abort`/`skip` are
recognised as actions, and the repository defaults to the caller's directory unless `path=` names one.
`git verify` keeps its own marshaller: its row precedes the wildcard, and the lookup takes the first
match.

Note that `cmd_git` in `src/cmd_infra.c` parses the same commands for the in-process (non-thin) path; the
two agree today but are separate parsers, and folding them together is untaken work.

### History integration: merge, rebase, sync, cherry_pick, revert

`src/modules/git/mcp_git_integrate.c` owns the operations that bring one line of history into another.
They are one operation with five names. Each can stop mid-flight on a conflict, each leaves a state that
must be continued or abandoned, and each commits and so needs an identity, so they share one driver and
differ only by a row in `OPS`.

They are modeled, not passed through, because the caller states the intent and aimee does the mechanics:

- a remote-looking ref is **fetched first**, so `merge origin/main` cannot merge a stale copy;
- the **editor is disabled** (`core.editor`/`sequence.editor`), because a blocked editor is
  indistinguishable from a hang;
- a **dirty tree is refused up front**, since a conflict in one cannot be cleanly undone;
- a conflict is reported as **the list of conflicted files**, and by default the operation is **aborted**,
  so the caller is never handed a half-applied tree it must know how to clean up. `abort_on_conflict=false`
  opts into resolving in place, then `action=continue` (which refuses while markers remain) or
  `action=abort`;
- a continuation **commits with the vaulted operator identity**, via the same `mcp_git_identity_flags`
  `git_commit` uses, because otherwise a conflict resolution would produce an authorless merge commit;
- an operation already in progress is named, with the way out, instead of letting a second one start on
  top of it;
- the result says **what changed** (`pre..post`, commit count, diffstat) rather than echoing git's prose.

`command=sync` is the whole "make this branch current with the branch it will merge into" errand in one
call: resolve the base (given, else origin's default branch, qualified to the remote's copy), fetch it,
rebase (default; `mode=merge`) it in, and report the gap it closed. Rebase is the default so a PR branch
reviews as the work alone.

Writing to `main`/`master` is refused here as everywhere else, so integrating into the default branch
remains `command=pr action=merge`.

### Staging and ref movement

`command=add` (`files`, or `all` to include new files) is the staging `git_commit` cannot reach: commit
stages tracked changes or the paths it was handed, so a new file had no route. Sensitive paths are
dropped, and for `all` the screen runs against the resulting **index** rather than the caller's argument
list, so a pattern-based add cannot slip a `.env` past `command=commit`.

`command=switch` and `command=checkout` are routing, not second implementations: `switch` is
`branch action=switch` (keeping the worktree lock and ownership registration in one place), and
`checkout` is `restore` when `files` is given and `switch` otherwise.

### Repository selection and raw-shell boundary

For MCP Git operations, an explicit `path` is repository identity, not a hint.
It is never redirected through stale session state or replaced by a fallback
checkout. A registered detached workspace routes Git to the client that owns
that path; a server-visible path runs directly; an inaccessible path fails
closed with a rebind/adopt/mount/serve explanation. This also permits Git
operations in another registered repository's managed worktree without relying
on shell working-directory persistence. `git_clone` is the exception because
its `path` is a destination that may not exist yet.

This repository-selection contract does not broaden raw Bash authority. The
attention guard's session-scratchpad carve-out and its parsing of compound
`cd`, redirects, and heredocs remain a separate subsystem; raw-shell behavior
outside registered/managed worktrees is intentionally deferred here. Git MCP
callers should pass `path` on each call instead of depending on a preceding
shell `cd`, since shell working directories are not session-persistent.

### `pr action=ready`: the whole "put this up for review" errand

Sync, push, open the PR. It exists because doing them separately makes the caller hold one piece of
knowledge it should not need (the sync rebases, which rewrites history, so the push after it must be a
lease-protected force), and because a failure halfway otherwise leaves the caller working out which step
broke. `ready` runs them in order, **stops at the first real failure and returns that step's own
explanation** (sync's conflict report already says how to resolve it), and otherwise reports each step on
its own line. It is idempotent: run it again after more commits and it re-syncs, re-pushes, and says the
PR is already open rather than failing.

Composition depends on one convention: a git tool reports failure in the text it returns, leading with
`error` or `conflict`. `mcp_git_response_failed` is the single definition of that check. A wrapper that
prefixed its own context to a failure (which `sync` originally did) makes a conflict read as success, so
`sync` now appends its context on failure and prepends it only on success.

### PR title and body are derived

`command=pr action=create` no longer requires `title`. When it is omitted, `pr_derive_from_commits`
writes both from the commits the branch has that the base does not: a single commit lends its subject and
its message body verbatim; several keep the conventional-commit prefix they agree on and get a bulleted
list plus the diffstat. Deterministic, with no model call: the commit subjects already are the summary, so
asking the caller to write one costs a turn to say the same thing. An explicit `title` always wins, and a
branch with no commits ahead of base is told exactly that.

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

The descriptor's fourteen direct tests are `test_forge_credentials.c`, `test_git_cred_inject.c`,
`test_git_forge_vault.c`, `test_git_host_resolve.c`, `test_git_ops.c`, `test_git_pr_ci_grade.c`,
`test_git_project.c`, `test_git_ssh_agent.c`, `test_git_verify_contract.c`, `test_git_verify_select.c`,
`test_mcp_git.c`, and the three CTest-only `test_git_oauth_device.c`, `test_git_oauth_gh.c`, and
`test_git_org_repos.c`. It is registered in `src/tests/CMakeLists.txt` but not built by a Make `unit-test-*`
target, so recorded as `make: false, ctest: true`, the inverse of the usual pattern. Two adjacent
tests are not claimed: `test_forge_app_token.c` exercises the root-level `src/forge_app_token.c`, not a
git-module source, and `test_forge_credentials_live.c` is the `forge-cred-live` integration harness
that needs a running forge. It provides supplementary coverage of `forge_credentials.c`, which already has a unit
test. Together with guardrail Git tests and integration tool calls they cover current behavior.
The Go handler, C parity fixture, and `test_git_ops` cover ref acceptance,
rejection, and missing-provider failure in addition to operation classification.
Missing repository, denied mutation, dirty/conflicting state, invalid ref/path, failed
signature/redaction, absent credential, forge error, or failed verify step must return typed failure;
non-Git base workspace operations continue normally.

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
