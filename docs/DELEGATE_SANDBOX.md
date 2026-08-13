# Delegate sandbox

Write-capable delegates run in an assigned worktree and, by default, a container with no network or
ambient credentials (`delegate_sandbox` is on unless an operator sets `delegate_sandbox: false`).
The sandbox bounds damage after a model or dependency makes a bad decision; it does not make
arbitrary host mounts safe.

## Default container posture

- network disabled;
- no Docker socket;
- no provider, vault, git, or host SSH credentials;
- workspace mounted at the declared root only;
- read-only system filesystem where the runtime permits it;
- bounded CPU, memory, process count, time, and output;
- explicit image and toolchain;
- cleanup and leaked-container reap.

The agent's role and workflow still decide whether the worktree is writable. A container is not a
write grant.

## Source authority

The server resolves the workspace and canonical worktree before launch. Absolute paths are accepted
only when they remain inside that root. Symlinks, `..`, alternate git worktree paths, and host mounts
cannot expand authority.

Container-bound worktrees are allowed outside the parent checkout when the managed worktree root
owns them. Arbitrary sibling paths are not.

## Packages and network

Delegates do not reach public package registries directly. Package requests use a mediated proxy or
prebuilt cache with allowlist, vulnerability, integrity, size, and audit policy.

If a task needs network, grant the narrow destination and protocol. Do not switch the whole backend
to host networking for one dependency.

## Images

An operator may choose:

- the default delegate image;
- a pinned custom image;
- an image extended with an approved package set;
- a reviewed Dockerfile built by the provisioning service;
- a learned toolchain image produced from verified project requirements.

The agent does not receive the host Docker socket or permission to replace the policy wrapper.
Record the final image digest with the job.

## Credentials

No credential is mounted by default. When one tool needs one credential, grant it through that tool's
contract and keep it out of the process environment where possible. The vault access is attributed
and audited.

Local CLI-provider logins stay on the thin client and do not enter the container.

## Isolation failure

Fail closed when the requested namespace, mount, network, or resource boundary cannot be created.
An operator may configure a documented degraded mode for a trusted host; every degraded launch emits
a sandbox audit event with the missing boundary.

Never silently fall back from container to host shell.

## A sandbox needs a workspace, and a bind source the daemon can see

Two conditions have to hold before a delegate can run a shell or read a file. Both fail quietly, and
both look like the model being unhelpful rather than like a deployment problem.

**The delegate needs an assigned worktree.** The container provider is bound per delegate from that
worktree; it is deliberately not selectable from configuration, because a configurable kind would
have to fall back to the shared provider when no container exists, which is the one outcome the
sandbox is for. A delegate launched with no workspace has nothing to containerise, falls through to
co-located execution, and is then refused:

```text
refused: a delegated shell requires sandbox isolation, but the sandbox is off/unavailable;
running unsandboxed on the aimee-server host is not permitted
```

That refusal is correct. The aimee-server process holds the Docker socket, so an unsandboxed shell
there is a host-root escalation. Give the delegate a workspace rather than reaching for
`sandbox.mode`.

**The bind source has to exist on the Docker daemon's host, not inside aimee-server.** When
aimee-server itself runs in a container and drives a sibling daemon through the mounted socket, a
path like `/var/lib/aimee/<workspace>` exists in its own filesystem and not on the host. Docker does
not fail on a missing bind source. It creates an empty directory and mounts that, so the delegate
gets a sandbox whose workspace is empty and every file tool answers `cannot open` for files that
plainly exist.

`AIMEE_SANDBOX_HOST_MOUNTS` carries the translation, as comma-separated
`<container-prefix>=<host-prefix>` pairs, longest match wins:

```text
/var/lib/aimee-workspaces=/var/lib/docker/volumes/aimee_aimee-server-workspaces/_data,/var/lib/aimee=/var/lib/docker/volumes/aimee_aimee-server-home/_data
```

The server entrypoint derives this from its own container's mounts at startup, so a compose
deployment needs no configuration. Set it explicitly only when the derivation cannot run, and check
the startup log to see which applied:

```text
[server-entrypoint] delegate sandbox: derived host-path map from own mounts (...)
```

Only the source is translated. The destination, which is the path the delegate sees, is left alone,
so absolute paths mean the same thing inside and outside the sandbox.

To confirm a real deployment rather than assume it, inspect a live delegate container and look at
where its workspace mount points:

```bash
docker inspect <aimee-delegate-...> --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{println}}{{end}}'
```

A source equal to the destination on a sibling-daemon host means the translation did not apply, and
that mount is an empty directory.

## No host fallback for delegates

A delegate runs in its allocated container or it does not run. The former INERT path was removed
by `89db96aebe`:
an unavailable container backend now fails allocation, and a delegated shell that reaches a
co-located path with `sandbox.mode=off` is refused before fork/exec. The trusted primary/operator
session remains a separate host-execution path; it is not a delegate fallback.

`delegate_sandbox_require_isolation` has the narrower job its name now documents: after a container
starts, verify that the runtime actually honored network isolation and fail closed if that proof is
missing or shows an attached network. It no longer decides whether a delegate may fall back to the
host; there is no such path. Package access, when enabled, crosses only the audited package proxy;
credential stripping and workspace guards remain defense in depth inside the container boundary.

Regression coverage lives in `test_server_compute` (container allocation is mandatory) and
`test_agent` (an active delegate is refused when a shell would otherwise run with sandbox mode off).

## Lifecycle

1. admit role, principal, budget, and agent slot;
2. resolve worktree and source authority;
3. select and verify image/toolchain;
4. construct mounts, limits, network, and package policy;
5. launch and record backend identity;
6. audit tool calls and completion;
7. stop, collect bounded results, remove container;
8. reap leaks after crashes or runtimes with weak filtering.

## Configure

Use [generated configuration](gen/configuration.md) for current sandbox, image, package, network,
resource, and worktree fields. Deployment-specific images and credentials belong in secret-aware
environment/configuration, not the repository.

See [Sandbox verification](DELEGATE_SANDBOX_VERIFY.md) before changing the posture.
