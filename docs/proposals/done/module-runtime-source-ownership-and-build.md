# Proposal: make module ownership drive source, builds, config, and documentation

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`module-runtime-source-ownership-and-build-residual.md`](../pending/module-runtime-source-ownership-and-build-residual.md).

- **State:** DONE — delivered scope archived 2026-07-26.
  C-core / separate-module-program boundary and the shared-memory event bus. The amendment adds the
  polyglot build, the event contract schema, the single in-source bus host and the bus-client spec
  with C and Go reference clients, bus ownership, and dependency enforcement over the bus; it reopens
  this child for re-review and does not inherit the 2026-07-20 approval.
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Owns:** descriptor schema/validation, physical source ownership, generated build inputs (C
  Make/CMake **and** per-language module program builds) and ownership-map outputs, the event contract
  schema, the single in-source (C) **bus host**, the language-neutral **bus-client spec** with its
  **C and Go reference clients** and a cross-language conformance suite, and bus ownership,
  include/type/symbol/link **and event publish/subscribe** dependency enforcement, and complete
  individual module-documentation gates
- **Implementation dependency:** feature-liveness dispositions identify what should move
- **Date:** 2026-07-20 (amended 2026-07-23)

## Decision

`src/modules/<name>/` becomes the authoritative home for feature implementation. A single validated
descriptor graph generates Make and CMake inputs, runtime registration, effective configuration,
profile selection, dependency reports, and module-documentation inventories.

## Physical boundary

Each module owns `module.yaml`, `include/aimee/<module>/`, private source, tests, and a corresponding
`docs/modules/<module>.md`. Application directories (`src/app/runtime`, `src/app/control`) own only
composition, listeners, lifecycle, and generic dispatch. `src/base`, `src/platform`, and generated
code remain deliberately small and may not absorb feature logic.

The migration removes feature implementation from `src/`, `src/server/`, `src/kb/`, `src/db1/`,
`src/modules/db2/c/`, and `src/headers/`. Database files move by owning capability, not by database number.
Temporary forwarding headers and root allowlists have owners, expiries, and may only shrink.

**Language boundary (2026-07-23 amendment).** Under the suite amendment, the communication core is C
and every module is a **separate program in any conforming language** (Go is the first-party
reference). `src/modules/<name>/` owns the module's source, `module.yaml`, an `eventcontract/` schema
directory (replacing `include/aimee/<module>/` public headers), tests, and `docs/modules/<module>.md`.
The C communication core lives under the application/composition roots and `src/base`/`src/platform`;
it exposes the shared-memory event bus and holds no feature logic. A module ships no public C header
and core links no module; core and modules are separate programs whose only cross-participant surface
is the event contract carried over the bus, so there is no cross-language linking and cgo is never
required. Trust-kernel modules (`vault`, `execution-policy`, `audit`) keep C source under the
communication core if the capability-contract child places them there; otherwise they follow the
module layout.

## Descriptor contract

Every descriptor declares identity, kind, required/optional state, default selection, runtime
toggle support, **hard and soft dependencies** (see below), required components, providers, source
and public-header globs, config ownership/read evidence, routes/commands/protocols, data ownership,
tests, docs, and compatibility aliases. Invalid, duplicate, cyclic, unowned, or incomplete
descriptors fail before
build generation.

Config read evidence is compiler-derived, not trusted descriptor prose. The inventory maps each key
to a compiled non-test read expression and its owning object/symbol; nonexistent files/lines,
string-only mentions, dead preprocessor branches, and unselected objects do not count. Required
modules may not read hidden startup enable flags. Every accepted legacy input names a live,
unexpired compatibility record in its descriptor.

This proposal owns the descriptor fields that declare config ownership and production read sites.
[`product-governance-web-and-config.md`](product-governance-web-and-config.md) exclusively owns how
those declarations become the rendered effective catalog, user-visible surfaces, activation
filtering, and config compatibility behavior.

**Surface declarations (2026-07-23 reconciliation).** The descriptor's routes/commands/protocols
declaration must be complete enough that a client which has never heard of the module can present
and dispatch its surfaces from the declaration alone: for a command, the verb and subcommand path,
the argument and flag schema with types and defaults, one-line and long help, tier, hidden flag, and
aliases; for a tool, the name, description, and JSON input schema; for a route, the method and path;
for a web surface, its identifier. Declarations are **declarative only** — a descriptor may not
declare executable content, a template language, a code reference, a client-side handler binding, or
a surface requiring client-local execution, and the validator rejects each. This proposal owns the
declarations; [`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md)
exclusively owns their projected wire form, the registration chain that carries them, and the
client-side compatibility rules — and its projection must be derived from these declarations, never
authored separately.

Optional modules are physically absent from the selected object and capability closure when
omitted. Runtime-toggle support is declared separately and requires registration, listeners,
routes, assets, config projection, and background work to unload cleanly. Required modules expose
no enable control. `workflows` is the sole owner of trigger, cron, and event-activation behavior;
there is no separate `triggers` module or registry.

`governance` is the sole optional owner of OIDC/SSO federation, organizational roles/policy
authoring, governance posture, governance approvals and decision records, attestation exports,
fleet-governance records, agent/delegation governance identity, and executable-artifact trust.
It may register policy and identity artifacts through core contracts but may not own or bypass core
execution authorization, audit-ledger integrity, vault custody, or gateway/protocol authentication.

`tests/baselines/modules/governance-ownership.yaml` is authoritative. It assigns distinct IDs for
OIDC federation, SSO federation, organizational roles, policy authoring, policy distribution,
governance approvals, governance decision records, posture profiles, attestation exports,
agent/delegation governance identity, fleet-governance records, artifact signing, and artifact
trust. It forbids aliases or ownership claims that shadow core `execution-policy`,
`policy.enforce`, `audit.ledger`, `vault.custody`, `gateway.auth`, or `protocols.auth`. It also requires
`governance` to declare dependencies on `module-runtime`, `config`, `ir`, `gateway`, `protocols`,
`vault`, `execution-policy`, `audit`, `routing`, `delegates`, and `tools`; core has no reciprocal
dependency.

The following is the normative semantic content of that future artifact; key order is irrelevant,
but no entry may be omitted, aliased, or weakened:

```yaml
schema_version: 1
module: governance
capabilities:
  oidc-federation: governance
  sso-federation: governance
  organizational-roles: governance
  policy-authoring: governance
  policy-distribution: governance
  governance-approvals: governance
  governance-decision-records: governance
  posture-profiles: governance
  attestation-exports: governance
  agent-delegation-governance-identity: governance
  fleet-governance-records: governance
  artifact-signing: governance
  artifact-trust: governance
forbidden_core_shadows:
  - execution-policy
  - policy.enforce
  - audit.ledger
  - vault.custody
  - gateway.auth
  - protocols.auth
required_dependencies:
  - module-runtime
  - config
  - ir
  - gateway
  - protocols
  - vault
  - execution-policy
  - audit
  - routing
  - delegates
  - tools
forbidden_dependencies_from_core:
  - governance
```

The ownership file and its checker are implementation deliverables of this proposal, not claims
that those files already exist. The descriptor validator must validate `schema_version: 1`, require
exact semantic equality with this block, reject unknown keys and aliases, and run before profile
generation. The suite's existing full-minus-one matrix then proves governance omission, while the
core-contract audit test proves the ledger remains required independently of governance.

## Event contract schema and the bus (2026-07-23 amendment)

`module-runtime` owns the shared-memory event bus and its schema registry. The bus has two sides —
the **bus host** and the **bus client** — and only one of them is a public, reimplementable spec:

- **The bus host is a single implementation in the core source, in C.** It owns the shared segment,
  the admission/handle handshake, observer routing, and the governance/audit tap. Only the two
  bus-hosting services contain it: the Runtime (server) and the Control Plane (kb), each hosting its
  own bus (suite invariant 16). It is core-internal; it is **not** published as a spec for anyone
  else to reimplement, and there is exactly one of it.
- **The bus client is a language-neutral spec with reference implementations in C and Go.** Because
  modules are separate programs in any language, the client side is the public contract: the wire
  spec ([`event-bus-wire-spec.md`](event-bus-wire-spec.md)) defines the segment/ring layout, the
  attach/admission handshake seen by a client, and the event encoding, and `module-runtime` ships a
  **C reference client** (for C-authored modules and core-adjacent bus clients) and a **Go reference
  client** (the first-party client every Go module links).

Two independent client references, **not one**, are what keep the bus-client spec honest: a
cross-language conformance suite runs shared wire-vector fixtures against both C and Go, and an
interop test drives the single in-source bus host with both a C and a Go client on one segment,
exchanging events in both directions. Passing the same vectors is what makes "any language" credible
— a third-language client is written against the spec and the vectors, not by reading a reference.
Each module declares its event contract in the descriptor: the event kinds it **publishes**, the
kinds it **subscribes to**, and the kinds it may **request** (correlated request/reply). The
generator emits, from those declarations, the typed event stubs and a publisher/subscriber edge map. An event kind has
exactly one owning publisher module; a request kind has exactly one serving module. The bus
authorizes and taps every event through the trust kernel (see
[`governance-attestable-enforcement.md`](governance-attestable-enforcement.md)); no module-to-module
path exists outside it.

`memory` is a sink: its descriptor may declare no request or subscribe edge to any other feature
module (shared invariant 14). Undeclared publish/subscribe, an event kind with two owners, a request
with no server, a cycle in the event edge graph, and any core→optional or module→`memory`-return
edge fail validation with descriptor/kind ownership evidence.

**Execution model (shared invariant 19).** Core and every module are separate programs; there is no
cross-language linking and no cgo. A trusted first-party module runs as its own process, mapped to
only its authorized bus queues. An untrusted external or user module runs under an enforced sandbox —
an OS-sandboxed process (seccomp/namespaces/container, any language) or a WebAssembly instance in a
host — reachable only through its authorized bus queues and unable to read core, `vault`, or another
module's memory. The sandbox mechanism is a deployment choice; the bus contract and admission are
identical across them. Native Go plugin loading is not used. `module-loader` owns artifact
verification, the sandbox host(s), and lifecycle.

**Bus admission (shared invariant 17).** `module-runtime` is the sole admission authority for its
service's shared-memory bus. The shared segment is not mappable by an arbitrary process: core grants
a module its handle and its own queue mappings only when the module is installed and registered, its
identity/artifact is attested — reusing the vault principal and `cert:CN`/bearer classes and, for
external modules, `governance` artifact trust, not a second scheme — and `execution-policy` authorizes
it. A refused module is not started and holds no handle; the refusal is audited. Admission is
least-privilege, so an admitted module still reaches only its declared, authorized event kinds.

**Subscription routing (shared invariant 18).** Delivery is observer-pattern, not broadcast.
`module-runtime` maintains, per event kind, the set of registered authorized observers and routes
each event only to that set; a request and its reply are delivered point-to-point to the serving
module and the requester. A subscription is admitted only when the descriptor declares the subscribe
edge and `execution-policy` authorizes it for the module's principal — there is no all-events
subscription for any module, and an undeclared or unauthorized subscription is refused, fail-closed
and audited. The sole full-stream observer is core's governance/audit tap, which is trust-kernel
infrastructure, not a feature module.

## Installation, capability publication, and replay (2026-07-23 amendment)

`module-runtime` owns installation as well as the bus. Three rules follow from the suite amendment:

- **Capability publication.** A module publishes its capabilities and state transitions to core over
  the bus at registration and as they change; core aggregates them into the capability closure and
  the advertisement projection
  ([`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md)). Core does
  not poll a module for its capabilities. A module that registers without publishing a capability
  contract, or that serves an event kind it never advertised, fails validation.
- **Dependency-complete installation (hard) and soft dependencies.** Each descriptor declares its
  module dependencies as **hard** or **soft**. The installer/profile generator computes the hard
  dependency closure and refuses — fail closed, naming the missing module — any install or selection
  that would leave a hard dependency uninstalled, and refuses any removal that would strand an
  installed hard dependent; this is an install-time precondition distinct from runtime readiness, and
  `memory`, as near-universal hard dependency, orders first. A **soft** dependency declares a
  capability the module *uses when present* plus a required fallback for when it is absent; it never
  blocks install, selection, or removal. The validator requires every soft dependency to name a
  fallback and forbids an optional module from declaring a **hard** dependency on another optional
  module (only a soft one). `module-loader`→`governance` artifact trust is the canonical soft edge.
- **Record and replay.** The bus supports capturing the per-service event stream and re-driving it.
  Two modes, per suite invariant 13: **observational replay** re-presents the recorded ordered stream
  (no re-execution, exact by construction), and **module replay** re-drives one module or a subset
  against its recorded inbound events and compares produced outbound events to the recording,
  **detecting and reporting divergence** rather than absorbing it. Determinism holds only to the
  extent a module is a function of its bus inputs; clocks, randomness, and external I/O must be sourced
  from the bus or injected from the recording, and a module that is not bit-reproducible is marked so.
  The suite makes no promise of bit-identical global re-execution across all processes. Capture obeys
  the audit tap's redaction and principal-scoping. Replay is the primary debugging and
  regression-seeding surface.

## Generated builds and dependencies

One deterministic generator emits sorted build inputs across languages: Make and CMake fragments for
the C communication core, and per-module build inputs (build recipe, generated event stubs, artifact
manifest) for each selected module program, whatever its language. It also emits
object/artifact-to-module, symbol-to-module, and event-kind-to-module maps. CI compares the selected
source/artifact/module sets byte-for-byte across build systems and every declared profile; the C
object closure and the module closure are each checked for drift.

Dependency enforcement combines two graphs. For the C core: compiler depfiles, preprocessor line
markers, public AST/type references, generated-header producer edges, and selected-object symbol
edges; public headers may include only declared public dependencies. For modules: the declared event
publish/subscribe/request edges, plus each language's own intra-module import check where available,
so a module may reach another participant only through the generated event stubs over the bus — never
by importing another module's source or linking its artifact. Across both: cross-module linkage,
undeclared edges, cycles, and core-to-optional edges fail with file/line/symbol or
descriptor/event-kind ownership evidence.

## Individual module documentation

Every module document must state purpose and non-goals, required/optional status, public contracts,
dependencies and consumers, providers and readiness, config and activation behavior, routes/
commands/protocols, owned data and migrations, security/privacy boundaries, supported journeys,
tests and failure behavior, operational diagnostics, compatibility aliases, and extension/removal
rules. Generated inventories are compared with the prose and require human owner attestation.
Attestation is a signed manifest containing module ID, document digest, owner identity, reviewer
identity, and timestamp; the mechanical docs gate validates its signature and digest.
Required sections must contain substantive content and reference real inventory IDs, symbols,
providers, config keys, surfaces, or data artifacts as applicable; headings and signatures alone
cannot satisfy the gate.

## DRY cleanup rule

Moving a file is not success. Each ownership-family change identifies duplicate implementations,
registries, representations, fallbacks, wrappers, and tests.
[`large-refactor-delivery-and-compatibility.md`](large-refactor-delivery-and-compatibility.md) owns
cleanup-ledger and compatibility mechanics; this proposal requires descriptors and docs to reflect
every approved consolidation or deletion.

Provider and alias duplication is checked on normalized AST/control-flow fingerprints after names,
paths, formatting, and compatibility shims are removed. A compatibility alias may call only the
canonical public entrypoint and translate declared surface values; it may not contain business
logic or a second provider implementation.

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/validate_module_descriptors.sh --schema src/modules/module.schema.json --all-modules --complete-ownership --no-duplicate-capabilities --no-cycles --no-unowned --no-incomplete --compiled-config-read-evidence --fail-fictional-dead-unselected-read-sites --fail-legacy-input-without-live-compat-record --forbid-required-module-startup-enable-flags --must-pass-before-generation"}
- {id: 2, tier: mechanical, check: "scripts/check_generated_module_builds.sh --make src/generated/modules.mk --cmake cmake/generated/modules.cmake --all-profiles --byte-equal --fail-drift"}
- {id: 3, tier: mechanical, check: "scripts/check_module_deps.sh --include-graph --public-symbol-graph --link-graph --no-cycles --no-core-to-optional --no-private-cross-imports --file-line-evidence"}
- {id: 4, tier: mechanical, check: "scripts/check_src_root_allowlist.sh src/ROOT_FILE_ALLOWLIST && scripts/check_forwarding_headers.sh --fail-migrated-consumers --enforce-expiry"}
- {id: 5, tier: mechanical, check: "scripts/check_module_docs.sh --catalog docs/modules --inventory build/inventory --required-sections --require-substantive-content --require-real-inventory-symbol-provider-config-surface-data-references --check-purpose-non-goals --check-public-contracts --check-dependencies-consumers --check-providers-readiness --check-config-activation --check-surfaces --check-data-migrations --check-security-privacy --check-journeys --check-tests-failures --check-diagnostics --check-compatibility --check-extension-removal --require-signed-human-attestation"}
- {id: 6, tier: mechanical, check: "scripts/check_capability_ownership.sh --catalog src/modules --require triggers=workflows,cron=workflows,event-activation=workflows --ownership-contract tests/baselines/modules/governance-ownership.yaml --schema-version 1 --require-normative-semantic-equality --reject-unknown-keys-aliases --require-exact-capability-owners --require-declared-dependencies --forbid-core-shadow --forbid-core-to-governance --must-pass-before-profile-generation --forbid-module triggers --forbid-parallel-registry"}
- {id: 7, tier: mechanical, check: "scripts/check_implementation_uniqueness.sh --normalized-ast-control-flow --fail-duplicate-providers --aliases-forward-only --forbid-alias-business-logic-second-implementation"}
- {id: 8, tier: mechanical, check: "scripts/check_module_inventory.sh --inventory tests/baselines/modules/canonical-inventory.yaml --schema-version 1 --required-count 18 --optional-count 8 --require-required-module git --forbid-optional-module git --require-code-intelligence-owner memory --require-capability-table-set-equality docs/proposals/pending/aimee-core-capability-contract.md --require-taxonomy-set-equality docs/proposals/pending/core-substrate-and-source-module-boundaries.md --forbid-module triggers,specialist-analyzers,delivery-channels,speech-adapters,sandbox-providers,additional-providers,extractors --forbid-parenthetical-module-ids --forbid-category-placeholders --must-pass-before-taxonomy-consuming-slices"}
- {id: 9, tier: mechanical, check: "scripts/check_proposal_ordering.sh --scan-module-status-claims docs/proposals,docs/modules --git-contract docs/proposals/pending/git-core-contract.md --require-accepted-before-source-path src/modules/git --require-accepted-before-descriptor-id git --require-accepted-before-build-registration git --require-accepted-before-profile-registration git --require-accepted-before-readiness git --require-child-acceptance-flags require-principal-scoping,require-signed-producer-and-repository-provenance,require-pre-persistence-secret-redaction --after-acceptance-require-descriptor-set-equality src/modules --after-acceptance-require-generated-profile-set-equality build/inventory --derive-order-only-from-descriptors --require-code-intelligence-owner-check docs/proposals/pending/memory-learning-and-inference-boundaries.md#binding-checks --must-pass-before-git-migration"}
- {id: 10, tier: mechanical, check: "scripts/check_language_boundary.sh --core-c --modules-any-language --forbid-core-to-module-link --forbid-cross-module-link --forbid-cgo --require-eventcontract-only-cross-participant-surface --trust-kernel-placement-from src/modules/aimee-core-capability-contract"}
- {id: 11, tier: mechanical, check: "scripts/check_event_contracts.sh --schema src/modules/eventcontract.schema.json --single-owner-per-event-kind --single-server-per-request-kind --no-cycles --no-core-to-optional --memory-is-sink --forbid-undeclared-publish-subscribe --forbid-non-bus-module-path --descriptor-kind-evidence"}
- {id: 12, tier: mechanical, check: "scripts/check_generated_module_builds.sh --module-closure --all-profiles --byte-equal --fail-drift && scripts/check_module_deps.sh --event-edge-graph --per-language-import-graph --no-cross-module-linkage --no-cycles --no-core-to-optional --file-line-and-event-kind-evidence"}
- {id: 13, tier: integration, check: "scripts/bench_event_bus.sh --baseline tests/baselines/bus/perf-budget.yaml --require-committed-baseline --metrics per-event-dispatch-overhead-ceiling,memory-roundtrip-p50,memory-roundtrip-p99,max-regression-factor --shared-memory-ring --zero-copy-payloads --no-syscall-on-fast-path --fail-if-baseline-absent-before-memory-migration --merge-gate --async-record-off-hot-path --synchronous-verdict-only-action-class"}
- {id: 14, tier: mechanical, check: "scripts/check_install_dependencies.sh --descriptor-declared-hard-and-soft-deps --transactional --refuse-install-with-unmet-hard-dep --refuse-remove-with-installed-hard-dependent --soft-dep-never-blocks-install-or-removal --require-soft-dep-fallback --forbid-optional-hard-dep-on-optional --name-missing-module --hard-dependency-closure --memory-orders-first --distinct-from-runtime-readiness"}
- {id: 15, tier: integration, check: "scripts/test_capability_publication.sh --modules-publish-to-core-over-bus --no-core-poll --aggregate-into-closure-and-advertisement --fail-register-without-capability-contract --fail-serve-unadvertised-event-kind"}
- {id: 16, tier: integration, check: "scripts/test_event_replay.sh --observational-replay-exact --module-replay-detects-and-reports-divergence --deterministic-only-as-function-of-bus-inputs --require-nondeterminism-sourced-from-bus-or-injected --mark-non-bit-reproducible-modules --no-global-lockstep-promise --capture-obeys-audit-redaction-and-principal-scope"}
- {id: 17, tier: mechanical, check: "scripts/check_user_module_boundary.sh --only-surface bus,event-contract,capability-publication --any-language --untrusted-principal --sandbox-os-process-or-wasm --reaches-only-authorized-bus-queues --memory-and-fault-isolated --events-authorized-by-execution-policy --recorded-by-audit --no-ambient-access --declared-event-kinds-only --dependency-must-be-installed --no-core-recompile-or-relink"}
- {id: 18, tier: integration, check: "scripts/test_bus_admission.sh --core-sole-admission-authority --shared-segment-not-mappable-by-arbitrary-process --grant-handle-and-queues-only-on-admission --require-attested-identity --reuse-vault-principal-cert-cn-and-governance-artifact-trust --verify-external-artifact-before-start --admit-only-installed-registered-authorized --least-privilege-declared-kinds-only --refused-module-not-started-holds-no-handle --fail-closed-and-audited"}
- {id: 19, tier: integration, check: "scripts/test_bus_routing.sh --observer-pattern --per-event-kind-observer-set --deliver-only-to-authorized-observers --module-maps-only-own-queues --no-all-events-subscription --request-reply-point-to-point --subscribe-requires-descriptor-edge-and-execution-policy --refuse-undeclared-or-unauthorized-subscription-fail-closed-audited --module-cannot-observe-or-enumerate-others-traffic --only-governance-audit-tap-sees-full-stream"}
- {id: 20, tier: integration, check: "scripts/test_execution_model.sh --separate-programs-no-cross-language-link --forbid-cgo --forbid-go-native-plugin --trusted-runs-own-process-own-queues --untrusted-os-sandbox-or-wasm --sandbox-cannot-read-core-vault-or-other-module-memory --module-crash-does-not-take-down-core-or-peers --independent-fault-domain"}
- {id: 21, tier: integration, check: "scripts/test_polyglot_module.sh --reference-clients c,go --wire-spec-conformance --build-non-reference-language-module --interoperates-over-bus --admitted-and-routed-like-any-module --proves-language-neutral-boundary"}
- {id: 22, tier: integration, check: "scripts/test_bus_reference_impls.sh --bus-host-single-in-source-c-used-by-runtime-and-control-only --bus-client-spec-public --c-reference-client --go-reference-client --both-clients-pass-shared-wire-vector-fixtures --bus-client-spec-validated-by-two-independent-clients-not-one --interop-single-bus-host-with-c-and-go-clients --bidirectional-event-exchange --third-language-client-from-spec-and-vectors-only --forbid-second-bus-host-implementation"}
```
