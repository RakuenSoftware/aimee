# Proposal: `module-loader` — load and host external and user-authored modules

- **State:** DRAFT — 2026-07-23; awaiting roundtable review. Later-drafted consuming child; it does
  not inherit any prior approval.
- **Parent:** [`core process separation residual`](core-process-separation-residual.md)
- **Owns:** the optional `module-loader` module — the module package format, artifact verification,
  the sandbox host runtimes (OS-sandboxed process and WebAssembly), and the loaded-module lifecycle
  by which an external or user-authored module is started, health-checked, upgraded, and stopped.
- **Consumes (does not own):** the shared-memory bus, admission authority, capability state, and
  routing (`module-runtime`/core); `execution-policy` authorization; `audit`; the bus-client spec and
  its C/Go reference clients; and, when selected, `governance` executable-artifact signing/trust
  ([`governance-agent-identity-and-artifact-trust.md`](governance-agent-identity-and-artifact-trust.md)).
- **Renames:** the taxonomy's former `plugin-loader` optional ID (suite 2026-07-23 amendment).
- **Date:** 2026-07-23

## Thesis

Under the modularization suite, core and every module are separate programs meeting only on the
shared-memory event bus, and a module may be written in any conforming language (suite invariants
11–12, 19). First-party modules are started as their own processes by ordinary orchestration. What is
still missing is the piece that takes an **external or user-authored** module — an artifact from
outside the build — verifies it, runs it under an enforced sandbox, and presents it to core for
admission. That piece is `module-loader`. It is renamed from the former `plugin-loader`: it loads
*modules* (native or sandboxed, any language), not in-process C plugins, and it supersedes the legacy
in-tree plugin loader.

`module-loader` is **optional**. A deployment that runs only built-in modules omits it entirely and
leaves no residue; omitting it simply means no external or user module can be loaded, and the
built-ins are unaffected.

## What it owns

- **Module package format.** A loadable module artifact bundles the module program (a native binary
  or a WebAssembly module), its descriptor (identity, declared event contract, dependencies, and
  sandbox kind), and a signed manifest. The format is versioned and language-neutral.
- **Artifact verification.** Before a module is started, its artifact is verified fail-closed. When
  `governance` is selected, verification uses its executable-artifact signing/trust; when governance
  is absent, `module-loader` enforces a trust-on-first-use hash-pin baseline. An unverified or
  tampered artifact is refused and the refusal is audited; `module-loader` never runs unverified code.
- **Sandbox host runtimes.** `module-loader` provides the two sandbox backends the suite permits for
  untrusted modules: an **OS-sandboxed process** (seccomp/namespaces/container) for a native binary in
  any language, and a **WebAssembly host** for a WASM-targeting module. The sandbox backend is a
  deployment choice; either way the module is reachable only through its authorized bus queues and
  cannot read core, `vault`, or another module's memory.
- **Loaded-module lifecycle.** Start, health-check, restart-on-crash, drain-and-upgrade, and stop.
  Because a loaded module is a separate program in its own sandbox, its crash is an isolated fault
  domain — it does not take down core or its peers — and it can be upgraded independently.

## What it does not own (the safety boundary)

`module-loader` starts and hosts modules; it does **not** admit them to the bus, authorize their
events, route their traffic, or hold the trust kernel. It presents a verified, started module and its
attested identity to core; **core** (`module-runtime`) is the sole admission authority and grants the
bus handle and queue mappings only after its own installation, identity, and `execution-policy`
checks (suite invariants 16–18). So `module-loader` cannot grant a module any access core has not
authorized: a compromised loader could start processes, but those processes still cannot map the bus
or exceed policy without core's admission. Artifact **signing authority** stays with `governance`;
`module-loader` verifies against it, it does not mint trust. Installation and dependency-completeness
stay with `module-runtime`; `module-loader` refuses to load a module whose install/dependency
preconditions are unmet, naming the missing dependency.

## Trust of the loader itself (optional, but not untrusted)

`module-loader` is optional — a deployment without external modules omits it — yet it hosts the
sandbox that confines *untrusted* code, which makes its own integrity trust-critical. Optional does
not mean untrusted: `module-loader` is **first-party code**, built and signed like the core, and its
artifact is attested before it runs; it is never itself a user-authored or externally supplied
module. Three properties keep a bug in the loader from becoming a sandbox escape:

- **Confinement rests on a hardened runtime, not loader-authored logic.** Isolation is enforced by
  the OS kernel (seccomp/namespaces/container) or a mature WebAssembly engine, not by
  `module-loader`'s own code. The loader configures and launches confinement; it does not implement
  the memory boundary itself.
- **Core admission is independent and still gates the bus.** Even a compromised loader cannot grant a
  hosted module bus access: `module-runtime` (core) is the sole admission authority and independently
  applies identity, installation, `execution-policy`, and per-queue routing. A module the loader
  starts reaches nothing until core admits it, and only its authorized queues thereafter. The loader
  cannot exceed policy on a module's behalf.
- **The loader is in scope for attestation.** Its integrity is measured and audited on the same
  footing as the core trust kernel; a deployment that requires signed modules also requires an
  attested loader, so a substituted or tampered loader is detectable.

This is defense in depth, not a claim that the loader is unimportant: it narrows a loader compromise
to "can start and stop sandboxes it is entitled to," not "can read `vault` or inject onto the bus."
Hardening and formal ownership of the confinement contract are `module-loader`'s to specify; the
core-side admission and policy gates are owned by `module-runtime` and hold regardless of the loader.

## Non-goals

- Owning the bus, admission, routing, capability state, or the trust kernel (all `module-runtime`/core).
- Owning artifact signing or organizational trust policy (optional `governance`).
- Owning the module taxonomy, descriptor schema, or the bus wire spec (the suite and `module-runtime`).
- Loading first-party built-in modules, which are started as their own processes by orchestration and
  need no loader.
- Reintroducing in-process native plugins or any cross-language linking; loaded modules are separate
  programs (suite invariant 19).

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_optional_module.sh --module module-loader --omit-leaves-no-residue --omit-blocks-external-user-modules-only --builtins-unaffected --renamed-from plugin-loader"}
- {id: 2, tier: integration, check: "scripts/test_module_artifact_verification.sh --package-format-versioned --fail-closed-on-unverified-or-tampered --use-governance-artifact-trust-when-selected --tofu-hash-pin-baseline-when-governance-absent --refusal-audited --never-run-unverified"}
- {id: 3, tier: integration, check: "scripts/test_module_sandbox_hosts.sh --backend os-sandboxed-process --backend wasm --reaches-only-authorized-bus-queues --cannot-read-core-vault-or-peer-memory --crash-is-isolated-fault-domain --deployment-selectable-backend"}
- {id: 4, tier: integration, check: "scripts/test_module_lifecycle.sh --start --health --restart-on-crash --drain-and-upgrade --stop --independent-per-module --crash-does-not-take-down-core-or-peers"}
- {id: 5, tier: integration, check: "scripts/test_loader_safety_boundary.sh --loader-not-admission-authority --core-grants-bus-handle-after-admission --loader-cannot-exceed-execution-policy --refuse-load-with-unmet-install-or-dependency --signing-authority-stays-governance"}
- {id: 6, tier: integration, check: "scripts/test_loader_trust.sh --loader-is-first-party-attested --confinement-by-os-kernel-or-wasm-engine-not-loader-code --core-admission-independent-of-loader --compromised-loader-cannot-grant-bus-access-or-read-vault --loader-in-attestation-scope --signed-module-deployment-requires-attested-loader"}
```

## Review status

Freshly drafted 2026-07-23. Not reviewed. It adds an optional module consuming core and (optionally)
governance contracts and modifies no parent or in-flight contract; the `plugin-loader`→`module-loader`
rename is recorded in the suite index amendment. If review finds this needs a change to a core or
governance contract, it must be re-scoped rather than amending those here.
