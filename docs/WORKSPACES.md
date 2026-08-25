# Workspaces

A workspace groups one or more repositories under one memory and code-graph scope. It is not a path
alias and does not grant write authority.

## Add a local workspace

```bash
cd /path/to/repo
aimee workspace add .
aimee workspace list
aimee index scan .
```

On a thin client, `workspace add` registers a detached workspace and uploads source content. The
server does not read the client path.

## Manifest

`aimee.workspace.yaml` names repositories and workspace metadata. Per-repository build, test, lint,
and risk rules live in `.aimee/project.yaml`.

Give a single-repository manifest a stable top-level identity, or give every repository in a
multi-repository manifest its own identity:

```yaml
id: billing-service
```

```yaml
repos:
  - id: billing-api
    url: https://github.com/example/billing-api.git
    path: repos/billing-api
  - id: billing-worker
    url: https://github.com/example/billing-worker.git
    path: repos/billing-worker
```

Stable IDs accept ASCII letters and numbers plus `. _ : / @ + -`, and must be shorter than 128
characters. Invalid or duplicate explicit IDs make the manifest invalid; they are not ignored.

Project identity is resolved in this order:

1. the matching explicit manifest ID;
2. the canonical forge remote and repository path; or
3. a generated UUID persisted in the repository's Git common directory, or `.aimee/project-id` for
   a non-git directory.

The persisted ID file is private (`0600`) and shared by linked worktrees through the Git common
directory. Checkout paths are aliases. Moving a checkout, renaming its parent directory, or using a
linked worktree therefore does not create a second project. If a stable identity cannot be resolved,
indexing fails instead of falling back to the directory basename.

## Cross-repo graph

All repositories in a workspace can share symbol, import, dependency, and memory edges. That lets
caller and blast-radius queries cross from a library into its consumers.

This shared graph does not make cross-project retrieval implicit. Ordered code and memory returns
start with the authenticated active project. Other projects are eligible only when the caller asks
for explicit all-project scope; the active project remains the first protected bucket.
Transports therefore preserve the active stable project even when `scope=all`; the project is a
ranking preference in that mode, not an exact-scope filter.

Exclude vendored trees, generated output, caches, and secrets before ingest. A shared workspace scope
should contain only knowledge appropriate for every member.

## Writes and worktrees

Sessions and delegates never operate from another session's checkout when isolation is required.
The launcher or workflow plane creates a managed worktree and branch before the client or container
starts. Read-only delegates receive a dedicated worktree too; its bind mount is read-only.

A new session gets its own branch and its own worktree at session start, not on first write. This
covers a Claude Code session through the SessionStart hook and an MCP session through
`aimee mcp-serve`. Both land at `<repo>/.aimee/worktrees/<key>/main` on branch
`aimee/session/<key>`.

At session start Aimee performs a bounded, non-interactive fetch and pins the exact commit returned
by the remote's current `HEAD`. A failed fetch stops the launch; stale remote-tracking state is not a
fallback. `session_worktree_base` may select a feature or release ref. If that ref already contains
the fetched default tip, Aimee changes nothing. Otherwise it merges the default tip only into the new
session branch, never into the user's source branch; a conflict stops the launch and removes the
empty failed session tree. `current` and `local_default` are the explicit offline/stale overrides.

`<key>` is derived from the whole session id, so two sessions never share a worktree or a branch. It
was previously the first 16 characters of the id, which collided for ids built on a shared prefix and
let concurrent sessions overwrite each other in one checkout. Aimee adds the internal worktree store
to the repository's local `info/exclude`; it does not change the project's committed `.gitignore` and
ordinary Git status stays clean.

`aimee launch -- <client>` allocates the session before `exec`, changes into the worktree, and then
starts any local executable. Client hooks are the compatibility path for clients launched directly:
they provision the same tree and route every supported file/shell tool into it while rejecting a
different session's tree. `aimee launch --gateway -- <client>` additionally routes provider traffic
through Aimee; workspace isolation itself does not require changing the client's model endpoint.

A delegate gets a sibling worktree containing its parent's branch and current working-tree state, so
it starts from what the parent has now rather than from the parent's last commit. Container creation
binds that exact directory and sets it as the container working directory. The runtime mount set is
inspected before the container is handed to the agent; a mismatch destroys the container. The
parent checkout's working files are not mounted into the container. Linked worktrees receive only
their common Git metadata (read-only) plus their own checkout; write roles additionally receive a
writable per-worktree index mount.

### Worktrees created before the key changed

A session that predates the key change owns a worktree under the old key. Session start reclaims it
once the new one is in place: the old worktree, its branch, and the emptied parent directory are
removed.

A stranded worktree holding uncommitted or unpushed work is kept, and the path is reported. Recover
that work by hand. Reclaim never removes a worktree that still holds changes.

Remote workspace control needs a full user grant. Index and document uploads need data. Workspace
registration can succeed with the read bearer, but its automatic ingest exits non-zero until the
user has data authority.

```bash
aimee worktree gc --dry-run
aimee worktree gc --days 14
```

Garbage collection skips active ownership. Use `--force` only after verifying the target is
abandoned.

## Remove

```bash
aimee workspace remove /path/to/repo
```

Removal unregisters the workspace. It does not silently delete source, git repositories, or durable
KB knowledge. `aimee index detach <stable-id>` hides the current generation while retaining it for
audit. `aimee index purge <stable-id>` and `aimee index gc` are separate owner-only, audited,
dry-run-first operations; each prints the exact manifest hash required for confirmation. See
[Code intelligence](CODE_INTELLIGENCE.md#project-generations-and-lifecycle) for the lifecycle
contract and commands.
