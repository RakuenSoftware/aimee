# Proposal: Per-project delegate sandbox image customization

- **State:** PENDING — design agreed, implementation to follow. Builds directly on
  the delegate sandbox (`delegate_sandbox`, `WS_PROVIDER_CONTAINER`,
  `delegate_backend_docker.c`, `workspace_turn_bind_container`) and on the
  server-side git routing fix (PR #1406) that removed the last reason the sandbox
  needed a network-facing binary of its own.
- **Author:** JBailes
- **Date:** 2026-07-16

## Thesis

The delegate sandbox runs `--network none` **by intent**: a delegate has no IP
egress, and its only outward channel is the bound `aimee-http.sock` to
aimee-server. Anything that needs the network or a credential (git, web search,
package installs) is performed **by aimee on the server's side**, never inside the
container. That is the correct boundary and it is now enforced (git was the last
tool wrongly executing in-container; PR #1406 routes it server-side).

What the sandbox still needs is the **local, network-free toolchain** for the
model's own work: a C project needs `gcc`/`make`, a Rust project `cargo`, a Python
project `python3`, a docs project nothing at all. Today every delegate runs on a
hardcoded `ubuntu:22.04` (`DOCKER_DEFAULT_IMAGE`) with no override wired
(`server_compute` binds the container with `image = NULL`), so it has coreutils and
nothing else — `verify`/`test`/build tools all fail. The toolchain is inherently
**per-user and per-project**; it cannot be one global image.

Because `--network none` removes the network at *run* time, the toolchain must be
baked in *before* that — at build time. aimee-server runs inside docker and drives
the host docker daemon, so `docker build` is always available: aimee can build a
per-project image itself, with network at build time, then run the delegate against
it with no network.

## Design

### Resolution chain (most specific wins), resolved at delegate-bind time from the delegate's cwd

1. **In-repo `.aimee/project.yaml` → `sandbox:` block.** The toolchain travels with
   the project; the code declares what it needs. (Primary "project" scope.)
2. **aimee.yaml per-workspace object → `sandbox`** on that workspace root. Operator
   override for a specific project root. (Workspace entries are already
   `{path, provider, …}` objects — this adds one field.)
3. **aimee.yaml global `delegate_sandbox_image` / `sandbox` default.** The per-user /
   per-instance default.
4. **Built-in base fallback.** A small `aimee-delegate-base` (or `ubuntu:22.04`).

### Spec forms (valid at any scope)

- `image: <ref>` — use a pre-baked image as-is (no build).
- `from: <base>` + `packages: [gcc, make, …]` — aimee generates a Dockerfile
  (`FROM <base>` + a single `apt-get install` layer) and builds it. Covers the
  common case in one line. **`packages:` is an apt shortcut** — it installs *system*
  packages on a Debian/Ubuntu base (including the language runtimes themselves:
  `nodejs`, `npm`, `python3`, `cargo`, `golang`). On a non-apt base (alpine, etc.),
  or for ecosystem dependencies, use `dockerfile:`.
- `dockerfile: <path>` — build a project-provided Dockerfile (escape hatch): any
  base, any package manager, and **ecosystem dependencies baked at build time** —
  `RUN npm ci`, `RUN pip install -r requirements.txt`, `RUN cargo fetch`. Build time
  has network, so these work directly.

### Where each package manager runs (apt / npm / pip / cargo / …)

Two distinct network contexts, and the answer differs by which one:

- **The tool binary** (`node`/`npm`/`python3`/`cargo`): a *system* package → apt
  `packages:` (or the base image already has it).
- **Ecosystem dependencies at BUILD time** (network available): a `dockerfile:`
  `RUN npm ci` / `pip install` / `cargo build` bakes them into the image.
- **Ecosystem installs at RUNTIME** (the model runs `npm install` mid-task, inside
  the `--network none` sandbox): these have no network and are the job of the
  **package proxy (below)**, which must therefore be **multi-ecosystem** — the apt
  mirror, the npm registry, PyPI, crates.io — not apt-only.

### Build + cache

- The derived image is tagged by content hash: `aimee-sbx:<sha256(base + spec)>`.
- Built **once** with network; reused across turns and delegates; rebuilt only when
  the spec hash changes. A short in-process lock avoids two concurrent turns racing
  the same build.
- Old `aimee-sbx:*` tags are pruned on an LRU/age policy.

### Invariant preserved

Build has network; the delegate **run** stays `--network none`. The credential and
the git/web rails remain server-side. The sandbox only ever gains **local,
network-free tools**.

### Trust

An in-repo `.aimee/project.yaml` lets a repository dictate what is built into its
own sandbox. This is acceptable: it is the co-located developer's own code, the
build is isolated in a throwaway layer, and the resulting container has no network.
It is nonetheless a trust surface — the same trust already extended to the repo's
`Makefile`/build scripts. Operators who want it locked down use scope 2/3
(aimee.yaml) and can disable in-repo specs.

## Prerequisite: the model's code execution must actually run in the sandbox

Audit finding (verified in `agent_tools_dispatch.c` / `posix/agent_tools.c:713`):
`bash` and `execute_script` route to the container provider ONLY for
`WS_PROVIDER_DETACHED`; for `WS_PROVIDER_CONTAINER` they fall through to a **local
fork on the aimee-server host** (`tool_bash`/`tool_execute_script`). So today a
sandboxed delegate's arbitrary shell/script runs on the host — with the host's
filesystem and the host's network — NOT inside the `--network none` container. The
file tools (`read_file`/`write_file` via `ws->read_all`/`write_all`) already route
in; the code-execution tools do not. This is a sandbox escape and it is also the
reason image customization is currently moot (the toolchain the model uses is the
*server's*, not the container's). **Fix this first:** route `bash`,
`execute_script`, `verify`'s command-run and the background-process tools through
the active provider for `CONTAINER`, exactly as the file tools do. Only after this
does the container's own toolchain matter.

## Package access: config-selected, aimee always the mediator

Agents need `apt`/`npm`/`pip`/`cargo`. The one invariant across every mode: the
delegate container is `--network none` and never holds a socket to the outside —
**aimee performs every fetch** and logs it (the delegate reaches aimee only over its
bound channel, exactly like git and web-search). What differs by config is *how much*
aimee is willing to fetch on the delegate's behalf. That spectrum runs from a full
proxy (the shipping default, for out-of-the-box functionality) to strictly airtight.

Two building blocks underlie the tighter modes:

- **Build-time installs (network in an isolated layer).** apt/npm/pip/cargo can run at
  `docker build` time (Phase 3) and be baked; the running delegate then needs no
  fetch at all.
- **Learned pre-bake.** A package a delegate needed is recorded and baked into the
  next build, so even the `off` mode converges to "everything already present."

### The runtime package-access policy is chosen BY CONFIG

A single config key — `delegate_sandbox_package_access` — selects the runtime
behaviour, so each operator picks their own point on the security/convenience curve.
Four modes:

- **`proxy` (default).** aimee proxies package-manager fetches to any host. This is
  the shipping default: it makes runtime installs "just work" out of the box. It IS a
  deliberate weakening — egress-via-aimee — accepted as the default for functionality;
  the delegate still holds no socket to the outside (aimee performs every fetch) and
  every fetch is logged, but it is not host-restricted. Operators who want a tighter
  posture select one of the modes below.
- **`off`.** No runtime proxy. Build-time installs + learned pre-bake only; the
  running sandbox is strictly `--network none`. The airtight posture, by config.
- **`gated`.** Host-allowlisted, registry-only proxy (deb mirror, `registry.npmjs.org`,
  `pypi.org`, `crates.io`, …); every fetch audited. **Off-allowlist pauses for human
  approval, routed to the primary session** — aimee forwards the authorization request
  over the delegate↔serving-session mailbox using the same approval-gate pattern as
  `computer_use`'s "off-allowlist … requires approval." Approve → host added to the
  (session or persistent) allowlist and the fetch proceeds; deny → refused.
- **`governance`.** The allowlist is supplied by a **governance provider** (an org
  policy source), not by interactive per-request approval. Off-allowlist is simply
  refused — governance, not the local operator or a live human, decides what a
  delegate may reach. This is the enterprise-policy hook for future governance work.

In every mode aimee performs the fetch (the delegate never holds a socket to the
outside) and every fetch is logged. `proxy` ships as the default for out-of-the-box
functionality; `off`/`gated`/`governance` are the config paths to a tighter boundary.

## Learned toolchain: customization from observed usage

aimee **records what each project actually needed** and feeds it back into the
build, so customization is largely **learned**, not authored. The signal comes from
three places, none of which require runtime egress:

- **Observed missing-package / failed-install signals (default).** When a delegate
  runs `apt install X` / `npm i Y` at runtime and it fails for lack of network, aimee
  captures the *intent* (the package name) and records it against the project.
- **The declared spec** in `.aimee/project.yaml` (an override / seed).
- **The opt-in proxy's audited fetches** (when enabled), which are already recorded.

Next build: aimee **pre-bakes the learned package set** into the project's
`aimee-sbx:<hash>` image (build with network, run `--network none`), so the tools are
present immediately — closing the loop without any runtime network. A failed runtime
install thus becomes "rebuild with this added," not "proxy it through."

## Phasing

1. **Sandbox-escape fix (prerequisite):** route `bash`/`execute_script`/`verify`
   command-run/background-process tools into the container for `WS_PROVIDER_CONTAINER`.
   The sandbox now actually contains code execution. *(done)*
2. **Image resolution (declared):** parse the `sandbox` block at all three scopes;
   resolve `image:`; wire it through `workspace_turn_bind_container`. *(done)*
3. **Build-from-spec:** `from`+`packages`/`dockerfile`; content-hash tag; build lock.
   *(done)*
4. **Learned toolchain (build-time loop, runtime stays airtight):** capture
   missing-package / failed-install intent at runtime; record per project; pre-bake
   the learned set into the next build. NO runtime proxy.
5. **Config-selectable runtime package-access proxy:** the `delegate_sandbox_package_access`
   modes — `off` (default), `proxy`, `gated` (allowlist + human approval via the
   primary session), `governance` (governance-provided allowlist). aimee always
   performs the fetch and logs it; `off` ships as the default.
6. **Cache management (renumbered):** record proxied/cached package sets per project;
   optionally
   pre-bake them into the project image. Declared spec becomes an override/seed.
6. **Cache management:** prune policy + `aimee delegate sandbox build/list/gc` CLI.

## Non-goals

- Giving the delegate its own IP egress (git/web/package fetch stay routed through
  aimee — the delegate never crosses the network boundary itself).
- A general devcontainer.json implementation (could be a later adapter onto this).
