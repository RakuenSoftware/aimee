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

Keep repository identity stable. Moving a checkout does not need to create a second project if the
origin and manifest identify the same repository.

## Cross-repo graph

All repositories in a workspace can share symbol, import, dependency, and memory edges. That lets
caller and blast-radius queries cross from a library into its consumers.

Exclude vendored trees, generated output, caches, and secrets before ingest. A shared workspace scope
should contain only knowledge appropriate for every member.

## Writes and worktrees

Sessions and delegates do not write directly into a shared base checkout when isolation is required.
The server or workflow plane creates a managed worktree and branch on first write.

Remote workspace mutation needs the full deployment posture and full user grant. Index/document
uploads need the data tier.

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
KB knowledge. Use explicit retention and purge operations for data deletion.
