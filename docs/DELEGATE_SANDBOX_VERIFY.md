# Sandbox verification

Verify the boundary from inside the launched delegate, not from the command used to create it.

## Required negative checks

The delegate must fail to:

- reach the public network or undeclared deployment services;
- read provider, git, vault, SSH, cloud, or Docker credentials;
- open the Docker socket;
- read or write outside the assigned workspace;
- escape through a symlink, `..`, alternate worktree path, or absolute path;
- mount a new filesystem or create a privileged namespace;
- exceed process, memory, CPU, output, or time limits;
- install an unapproved package or bypass the package proxy;
- keep a container/process alive after cancellation or daemon restart.

## Required positive checks

The delegate must be able to:

- read the intended source;
- write only when the role grants it;
- run the declared compiler/test tools;
- use approved cached packages;
- produce a commit in its own worktree;
- return bounded logs and results;
- shut down normally and release admission.

## Fail-closed checks

Force network inspect, mount inspect, environment inspect, bus-socket validation, and refused-container
removal to fail independently. Every launch must refuse and must never execute on the host. Also resume
a stopped container with a stale mount or environment and confirm the same verification runs again.

## Cleanup check

Kill the delegate, runtime, and server at different points. After the configured reap interval:

- no process/container remains;
- no agent slot remains held;
- no worktree is marked active by the dead attempt;
- no arena or event-bus client reference remains;
- the durable job records interruption rather than success.

## Image and package check

- verify the image digest recorded on the job;
- mutate a pinned executable and confirm the build/launch refuses it;
- request a blocked or vulnerable package and confirm denial;
- corrupt a cached package and confirm its digest is checked;
- run with the registry offline and confirm only verified cache entries work.

## Sanitizers and races

Run the narrow sandbox/backend tests under ASAN/UBSAN. Run concurrency, cancellation, cleanup, and
event publication under TSAN where supported. Container integration tests still matter; a unit test
cannot prove the host runtime applied a namespace or mount.

Record runtime version, kernel, image digest, config, commands, and results with the verification.
