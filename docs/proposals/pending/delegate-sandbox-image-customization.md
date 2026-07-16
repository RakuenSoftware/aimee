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
  common case in one line.
- `dockerfile: <path>` — build a project-provided Dockerfile (escape hatch).

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

## Package access without network: aimee as the package proxy + cache

The sandbox stays `--network none`; its only channel is the bound aimee socket. Yet
agents legitimately need `apt`/`npm`/`pip`/`cargo` to install a toolchain. Rather
than pre-bake everything, **route package managers through aimee**: point `apt`,
`npm`, `pip`, etc. inside the container at aimee as their mirror/proxy (over the
bound channel — a loopback shim forwarding to the UDS, or an aimee-served proxy
endpoint). aimee (which has network) fetches upstream, **caches the artifacts**, and
serves them back. The delegate installs what it needs; the network boundary is
never crossed by the delegate itself — only by aimee, in the same way git and web
search already work.

## Learned toolchain: customization from observed usage

Because every package install flows through aimee's proxy+cache, aimee **records
what each project actually routed and cached**. That observed set *is* the project's
toolchain. So image customization is largely **learned**, not authored:

- First runs: the delegate `apt install`s what it needs; aimee proxies + caches +
  records it against the project.
- Later runs: aimee can **pre-bake the learned package set** into the project's
  `aimee-sbx:<hash>` image (build with network, run `--network none`), so the tools
  are present immediately and installs become cache hits.

The declared `.aimee/project.yaml` / aimee.yaml `sandbox` spec from the section
above is then an **optional override / seed**, not a requirement — the common path
is that aimee figures the toolchain out from real usage.

## Phasing

1. **Sandbox-escape fix (prerequisite):** route `bash`/`execute_script`/`verify`
   command-run/background-process tools into the container for `WS_PROVIDER_CONTAINER`.
   The sandbox now actually contains code execution.
2. **Image resolution (declared):** parse the `sandbox` block at all three scopes;
   resolve `image:`; wire it through `workspace_turn_bind_container` (replace the
   `NULL` at `server_compute.c:1396`). Named pre-baked images work end-to-end.
3. **Build-from-spec:** `from`+`packages`/`dockerfile`; content-hash tag; build lock.
4. **Package proxy + cache:** aimee serves `apt`/`npm`/`pip` fetches over the bound
   channel and caches artifacts; container package managers are pointed at it.
5. **Learned toolchain:** record proxied/cached package sets per project; optionally
   pre-bake them into the project image. Declared spec becomes an override/seed.
6. **Cache management:** prune policy + `aimee delegate sandbox build/list/gc` CLI.

## Non-goals

- Giving the delegate its own IP egress (git/web/package fetch stay routed through
  aimee — the delegate never crosses the network boundary itself).
- A general devcontainer.json implementation (could be a later adapter onto this).
