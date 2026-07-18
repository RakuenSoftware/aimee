# Verifying in the delegate sandbox

> **Autonomous verify runs where the toolchain is.** When the workflow engine
> gates an `implement` unit on `aimee git verify`, the verify steps now run
> **inside the delegate's `--network none` sandbox** — the same per-project,
> toolchain-baked image the engineer delegate builds against — instead of in the
> `aimee-server` process. The slim server runtime image ships no `gcc`/`make`/
> `node`, so an in-process verify there can never build a real project; running
> verify in the sandbox is what lets an autonomous run reach a green gate.

## Why this exists

An autonomous `build` run decomposes a proposal into slices and, for each slice,
runs `implement → freeze → roundtable → PR → CI → merge`. The `implement` block
ends with a **mandatory mechanical verify** (`wfe_implement_verify_ok`): the
change must pass `aimee git verify` before it can advance, and the gate is
**fail-closed** — if verify cannot run, the unit does not advance, it loops.

The `aimee-server` binary ships in a **slim runtime image** (the final stage of
`Dockerfile.server` is `debian:bookworm-slim` with the compiled binaries and
runtime libraries only — no compiler, no build tools). Running the verify steps
in that process therefore fails for any project that has to be compiled or
tested: the build tool isn't there. Fail-closed then turns into an **infinite
re-implement loop** that burns the unit's wall-clock budget and parks the run,
never reaching the review roundtable.

The delegate **sandbox** is the environment that *does* carry the project
toolchain: a `--network none` container whose image is resolved per project and
whose build tools are baked in (see [Delegates](DELEGATES.md) and the delegate
sandbox image spec below). Verify belongs there, next to the toolchain and the
worktree the delegate just edited — not in the daemon.

## How it works

When the live verify provider runs a work item's mechanical gate
(`wfe_live_delegate.c`):

1. **Resolve the verify steps** for the worktree (`verify_load_config`) — from
   the project's `verify.yaml`/`project.yaml`, or auto-generated from a
   recognized build file.
2. **Resolve the sandbox image** for the worktree
   (`delegate_sandbox_resolve_image`) — honoring a per-project `sandbox:` spec
   (below), falling back to the backend default.
3. **Acquire the sandbox** through the `docker` delegate backend
   (`--network none`, worktree bind-mounted, the package-egress proxy armed).
4. **Run each verify step inside the sandbox** (`exec`), collecting each step's
   exit code.
5. **Synthesize the verdict** — `{"verdict":"passed"|"failed","steps":[…]}` — the
   same shape the implement gate consumes.

If no sandbox is available — no `docker` backend registered, no resolvable verify
steps, or the sandbox fails to start — verify **falls back to the in-process
gate**, so the CLI (`aimee git verify`) and non-sandboxed deployments are
unchanged.

## Specifying a custom base image

The sandbox toolchain is **per project**, because a Rust repo needs `cargo`, a C
repo needs `gcc`/`make`, and a docs repo needs nothing. Declare it in the
project's sandbox spec (resolved most-specific-first: project, then workspace,
then the global default). Three forms:

```yaml
# a pre-baked image, used as-is (you maintain the toolchain)
sandbox:
  image: ghcr.io/acme/build-tools:latest

# aimee builds a derived image: your base + these apt packages
sandbox:
  from: ubuntu:24.04
  packages: [build-essential, clang, make, pkg-config]

# aimee builds this Dockerfile (repo-relative or absolute)
sandbox:
  dockerfile: .aimee/sandbox.Dockerfile
```

Point `from`/`image` at whatever base you like and aimee bakes your dependencies
into the sandbox image; verify then runs against exactly that toolchain.

## Learned dependencies

You do not have to enumerate every package up front. When a verify step (or a
delegate) runs `apt-get install <pkgs>` inside the sandbox, aimee **captures the
package names** (`sandbox_learned_observe`) and records them against the project.
The **next** sandbox image build pre-bakes the learned set in, so the tools are
present immediately with no runtime install.

This is the intended **B → A** progression:

- **B (first runs):** the sandbox has the package-egress proxy, so a step that
  installs a missing build dependency succeeds over aimee's egress.
- **A (steady state):** those installs are learned and pre-baked, so subsequent
  verify runs need no network for dependencies — they run against the baked-in
  toolchain offline.

The learned set is a best-effort JSON sidecar under `$AIMEE_HOME`
(`sandbox-learned.json`); it only recognizes `apt`/`apt-get install`, matching
the apt-based image builder.

## Prerequisites and limits

- **A resolvable verify config.** Verify runs the steps `verify_load_config`
  resolves. A repo whose build file the auto-generator does not recognize (for
  example, aimee's own Makefile lives at `src/Makefile`, not the repo root) needs
  an explicit `verify.yaml`/`project.yaml` declaring its steps, or there are no
  steps to run and verify has nothing to gate on.
- **The `docker` delegate backend must be engaged.** The sandbox path is used
  when the `docker` backend is registered and a sandbox can be acquired for the
  worktree. A deployment whose delegates run on the in-process/`local` backend
  does not build a sandbox, so verify falls back to in-process (and inherits that
  environment's toolchain, or lack of one).
- **The verdict is pass/fail by step exit code.** A step's non-zero exit — or a
  transport failure acquiring/exec'ing in the sandbox — counts as a failed step;
  any failed step fails the verdict.

## See also

- [Autonomous Development](AUTONOMOUS_DEVELOPMENT.md) — the implement → verify →
  re-delegate loop the mechanical gate lives in.
- [Workflows](WORKFLOWS.md) — the `implement`/`slice` blocks and their gates.
- [Delegates](DELEGATES.md) — delegate execution backends and the sandbox.
