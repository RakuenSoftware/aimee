# Delegate Sandbox

When `delegate_sandbox` is enabled, a server-executed delegate — a coord job,
a roundtable panelist, or an autonomy run, anything that gets its own worktree —
runs its file and shell/script tools inside a Docker container instead of in
aimee-server's own process. This page covers what that container can reach and
how you pick the image it runs.

Turn it on with the `delegate_sandbox` config key (off by default); it needs a
reachable Docker daemon. See the key's entry in the
[configuration reference](gen/configuration.md).

## The execution model

The sandbox container runs `--network none`. The delegate has no IP egress. Its
only outward channel is the bound `aimee-http.sock` back to aimee-server.

Anything that needs the network or a credential — git, web search, package
fetches — runs **server-side, on the delegate's behalf**, never inside the
container. aimee performs those operations itself and hands the delegate the
result over the bound socket. What stays inside the container is the delegate's
own local work: reading and writing files in its worktree, and running build,
test, and `verify` commands.

That has one consequence you have to plan for: **the toolchain must already be
in the image.** A `--network none` container cannot install anything at run
time. A Rust repo needs `cargo` baked in, a C repo needs `gcc`/`make`, a docs
repo needs nothing. Because that toolchain is per-project, the image is resolved
per delegate.

## How the image is chosen

At the point a delegate is bound to its container, aimee resolves an image from
the delegate's working directory, **most specific first**:

1. The repo's `<git-root>/.aimee/project.yaml` `sandbox:` block — the toolchain
   travels with the code that needs it.
2. A per-workspace `sandbox_image` override in `aimee.yaml`, for the workspace
   whose root contains the delegate's directory (longest matching root wins).
3. The global `delegate_sandbox_image` in `aimee.yaml`.
4. The built-in default, `ubuntu:22.04`.

The first scope that yields a usable image wins. A `.aimee/project.yaml` spec
that is declared but fails to build does **not** silently fall through to a
lower scope's image with no signal — the build failure surfaces through Docker's
own build logs.

## The `.aimee/project.yaml` `sandbox:` block

The in-repo block is the primary, per-project control. It takes one of three
forms.

### `image:` — use a pre-baked image as-is

```yaml
sandbox:
  image: ghcr.io/acme/ci-toolchain:2026-06
```

aimee runs that image directly. No build step.

### `from:` + `packages:` — build a derived image

```yaml
sandbox:
  from: ubuntu:22.04
  packages: [gcc, make, python3]
```

aimee generates a two-line Dockerfile — `FROM <base>` plus a single
`apt-get install` layer — and builds it.

`packages:` is an **apt shortcut**: it works only on a Debian/Ubuntu base and
installs *system* packages. That includes the language runtimes themselves —
`nodejs`, `npm`, `python3`, `cargo`, `golang`. For a non-apt base (Alpine and
the like), or to bake in ecosystem dependencies, use `dockerfile:` instead.

Package names are validated against `[A-Za-z0-9][A-Za-z0-9._+:-]*`. A name
containing shell metacharacters is rejected and the spec does not build.

### `dockerfile:` — build a project-provided Dockerfile

```yaml
sandbox:
  dockerfile: .aimee/sandbox.Dockerfile
```

The escape hatch: any base, any package manager, and — because a build has
network access — ecosystem dependencies baked at build time.

```dockerfile
FROM node:22-slim
WORKDIR /app
COPY package*.json ./
RUN npm ci
```

The path is resolved relative to the git root (or taken as-is if absolute).

### Build and cache behavior

For the `from:`+`packages:` and `dockerfile:` forms, aimee runs `docker build`
with network available at build time and tags the result by the content hash of
its Dockerfile: `aimee-sbx:<hash>`. The build happens **once** and is reused
across turns and delegates; it only rebuilds when the spec changes. The delegate
then **runs** that image `--network none`.

## `aimee.yaml` scopes

The operator-facing overrides in `aimee.yaml` accept the `image:` (pre-baked)
form only — a bare image reference. They do not build from a spec; use
`.aimee/project.yaml` for that.

### Per-workspace override

A workspace entry can be a bare path string or a `{path, ...}` object; add a
`sandbox_image` field (an object entry — a bare string carries no override):

```yaml
workspaces:
  - path: /srv/repos/backend
    sandbox_image: ghcr.io/acme/rust-toolchain:1.81
```

The override applies to a delegate whose directory is at or under that
workspace root. When roots nest, the longest matching root wins.

### Global default

A top-level key sets the default for every delegate not covered by a more
specific scope:

```yaml
delegate_sandbox_image: ghcr.io/acme/base-toolchain:latest
```

## Worked example

A C project that needs a compiler, `make`, and Python for a test harness — and
nothing from the network at run time:

`<repo>/.aimee/project.yaml`

```yaml
sandbox:
  from: ubuntu:22.04
  packages: [gcc, make, python3, python3-pytest]

# (verify steps, env_check, etc. as documented in agent.md)
verify:
  steps:
    - name: build
      run: make
    - name: test
      run: python3 -m pytest
```

On the delegate's first turn aimee builds `FROM ubuntu:22.04` plus the four
packages, tags it `aimee-sbx:<hash>`, and runs the delegate against it
`--network none`. Later turns reuse the built image until the `sandbox:` block
changes.

## Installing dependencies: build time, not run time

Because the running sandbox is `--network none`, install every toolchain and
dependency at **build time**:

- System tools and language runtimes → `packages:` (apt) or a `dockerfile:`.
- Ecosystem dependencies (`npm ci`, `pip install -r requirements.txt`,
  `cargo fetch`) → a `dockerfile:` `RUN` step, which has network during the
  build.

A config-selectable **runtime package-access policy**
(`delegate_sandbox_package_access`) — which would let aimee proxy package-manager
fetches on the delegate's behalf while the container itself stays network-none —
is designed but **not yet shipped**. See the
[proposal](proposals/pending/delegate-sandbox-image-customization.md) for the
planned modes. Until it ships, bake what the delegate needs into the image.

## See also

- [`.aimee/project.yaml` conventions and `verify` steps](agent.md)
- [Delegates: roster, routing, and economics](DELEGATES.md)
- [Configuration reference](gen/configuration.md) — `delegate_sandbox`,
  `delegate_sandbox_image`, and the workspaces `sandbox_image` key
- [Design proposal](proposals/pending/delegate-sandbox-image-customization.md)
