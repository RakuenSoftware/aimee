# Delegate sandbox

Write-capable delegates run in an assigned worktree and, when configured, a container with no
network or ambient credentials. The sandbox bounds damage after a model or dependency makes a bad
decision; it does not make arbitrary host mounts safe.

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
