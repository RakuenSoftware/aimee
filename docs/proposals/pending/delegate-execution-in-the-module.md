# Delegate execution moves into the module

Status: pending
Owner: delegates

## The design

A delegate is a completely sandboxed container. That is the whole of it:

```
  container: agent loop + tools + file edits
     │  (only outward path)
     ▼
  parent module proxy ──► LLM traffic
                     └──► very limited whitelist: software updates
     │
     ▼
  event bus / egress proxy module
```

- Files arrive by **bind mount**. A role that writes gets its own branch and
  worktree, mounted read-write. A role that does not write mounts the
  **parent's** worktree, permission-enforced read-only.
- Which mount a delegate gets is already decided in Go: `RoleIsWrite` at stage
  10 / event 6666. There is no separate isolation question to invent.
- The delegate has **no network** except the one proxy to its parent module.
- The agent loop runs **inside** the container. Its LLM calls leave through the
  parent proxy, then the module, then egress.
- The container holds **no credentials of any kind**. Work that needs them is
  done by a module on the delegate's behalf — git through the git module,
  secrets through the vault module — each reached over the bus.
- When the delegate finishes the container exits, and the parent already has the
  edited files, because the worktree is a bind mount.

## What this design does NOT need

The current C carries machinery for problems this design does not have. Naming
them is the point of this document, because each is a deletion rather than a
port:

| Machinery | Why it goes |
|---|---|
| result shipping / diff transport | the bind mount IS the transport |
| in-container credentials | the container never authenticates |
| ssh backend | a delegate is a container |
| local backend | a delegate is a container |

## What must be preserved

The C is accretion in structure, but it encodes constraints that were measured,
not assumed. These are not optional and must survive the move:

1. `--network none`. The container's only reachable peer is the parent, over a
   bound unix socket.
2. **Never** bind `/var/run/docker.sock`. That hands a delegate root-equivalent
   control of the host daemon.
3. Refuse to bind-mount a directory that is not a git checkout. Otherwise any
   host directory can be mounted into a delegate.
4. Mount layering for a write delegate, validated on docker 26.1.5:
   - `<repo>:ro` — the tree is readable, and a write outside the worktree fails
     with "Read-only file system"
   - `<worktree>` read-write — nested mount correctly overlays the read-only repo
   - `<gitdir>` read-write — `git status` refreshes its index there, so a
     read-only `.git` breaks it
5. A consequence of (4) is that `git commit` inside the container fails: blobs
   cannot be written to a read-only object store. This is intended. Commits run
   module-side, which is the same rule as "credentialed work is done by
   modules".
6. Docker resolves bind **sources** in the daemon's namespace, not the calling
   process's. When aimee itself runs in a container the two differ, and an
   untranslated source silently mounts the wrong directory.
7. Isolation is **verified at runtime**, not assumed: ask the host daemon for
   the container's attached networks and confirm none is attached and none has
   an IP.
8. `http_proxy` points at the egress so a `--network none` delegate can still
   install software — the narrow update whitelist, and nothing broader.

## Why read-only must be enforced rather than trusted

A read-only delegate mounts the **parent's** worktree — the branch a supervisor
is actively working in. If the mount is writable and the delegate is merely
asked not to write, a review or diagnose delegate can corrupt the supervisor's
branch. The mode is the enforcement; the role is only what selects it.

## Order of work

1. **Sandbox specification** (this slice). A pure function: given a role and the
   workspace, produce the container specification — mounts and their modes,
   network posture, environment. No I/O, fully testable, and the place where
   every invariant above is stated once.
2. Container lifecycle in Go, driven by that specification.
3. The parent proxy: the single outward channel.
4. Retire the C driver and backends, deleting rather than translating the
   machinery listed above.

Sequencing matters: the specification is where the safety properties live, so it
is written and tested before anything creates a container.
