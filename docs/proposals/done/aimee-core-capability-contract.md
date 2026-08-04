# Proposal: define Aimee's required core capability contract

- **State:** DONE — archived 2026-08-04 as partially implemented. The capability taxonomy,
  descriptors, source ownership, and initial module-event adapters landed; the required
  cross-process round trip remains in
  [`aimee-core-cross-process-contract-residual.md`](../pending/aimee-core-cross-process-contract-residual.md).

- **Historical state:** roundtable-approved 2026-07-20; **amended 2026-07-23 (post-approval)** for the
  C-core / separate-module-program boundary and the shared-memory event bus. This child now owns the
  final core-vs-module carving of the eighteen IDs and a round-trip proof whose stages flow as bus
  events across the boundary. The amendment reopens this child for re-review and does not inherit the
  2026-07-20 approval.
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries-residual.md)
- **Owns:** required module responsibilities, core dependency law, the core-vs-module carving of the
  required set, and the executable core proof
- **Implementation dependency:** module descriptor/build enforcement
- **Date:** 2026-07-20 (amended 2026-07-23)

## Decision

Aimee Core consists of eighteen required capabilities. Each owns a narrow public contract and one
working reference implementation. None imports, links, loads, or requires an optional module.

**Language carving (2026-07-23 amendment).** Under the suite amendment, "required" no longer means
"compiled into the C core." The eighteen required IDs split across the C-core / Go-module axis:

- **Communication core (C):** `module-runtime` (which also owns the shared-memory event bus and
  capability-state authority), `config`, `ir`, `translation`, `protocols`, and `gateway`.
- **Required modules (Go reference):** `memory`, `learning`, `routing`, `delegates`, `tools`,
  `workspace`, `git`, `skills`, and `response-composition` — always selected, separate programs
  reached only over the bus, authored in Go as the first-party reference but admitting any conforming
  language.
- **Trust kernel:** `vault`, `execution-policy`, and `audit`. This proposal places them in the **C
  communication core**, because the bus authorizes and records every inter-module event through them
  and the safety boundary must not depend on a module the bus is trying to reach. Wherever a future
  amendment might move them, their contracts and reference implementations stay required in every
  profile. This placement is the carving the suite delegated here; it is normative for this child's
  proof and binding checks.

The dependency law below is unchanged in intent but is now evaluated over event-contract edges, not C
link edges: no required capability requests or subscribes to an optional module, and `memory` is a
sink that requests nothing from another feature module. Core and modules are separate programs, so no
cross-language linking or cgo exists on any of these edges.

| Module | Required responsibility |
|---|---|
| `module-runtime` | descriptor catalog, dependency resolution, registration, lifecycle, capability state, readiness |
| `config` | validated effective configuration and truthful projections |
| `ir` | canonical request, response, event, tool, and stream-delta values and stages |
| `translation` | typed conversion between IR and provider/application/transport representations |
| `protocols` | required MCP and ACP client/server mappings |
| `gateway` | admission, sessions, streaming, cancellation, IR ingress/egress, response delivery |
| `memory` | storage, structured extraction/indexing, embedding, reranking, recall, and code intelligence |
| `learning` | provenance-aware recall and recording of user preferences, corrections, approaches, and outcomes |
| `routing` | selection of models, agents, tools, typed optional target kinds, memory sources, and destinations |
| `delegates` | agent execution, lifecycle, invocation, and cancellation |
| `tools` | typed discovery, schema validation, authorization, dispatch, and result handling |
| `workspace` | scoped resource access and a local coding-agent provider |
| `git` | reads repository state and history and produces records through memory's public ingest boundary; memory retains schema, provenance, storage, indexing, retrieval, and code intelligence |
| `skills` | procedural-memory discovery, validation, matching, application, provenance, snapshots, rollback |
| `response-composition` | final canonical response, summary, and citations from ranked evidence and request context |
| `vault` | principal-scoped secret custody, encryption, injection, and rotation |
| `execution-policy` | fail-closed authorization for delegate and tool actions; inability to decide denies execution |
| `audit` | typed append-only security/action events with verifiable ordering, tamper evidence, and a working local ledger |

Application composition roots, base value types, platform shims, and generated contracts are core
infrastructure, not feature modules. Optional provider implementations may extend core contracts;
core depends only on their public interfaces and always retains a usable reference provider.

## Dependency law

Core dependencies must be declared, acyclic, and limited to public module headers. Public headers
may not import another module's private headers. Optional modules may depend on core; core may not
depend on optional modules. Provider registries are owned by the consuming core contract, not a
generic service locator. The generated dependency graph is authoritative for both build systems.

Security boundaries are not optional. The vault backend, execution-policy implementation, audit
ledger, workspace provider, and delegate executor may be replaced, but their contracts and a
working local/software implementation remain in every core profile.

Git is required because repository state and history are inputs to core workspace behavior and
memory-owned code intelligence. This taxonomy amendment does not invent a new Git API, event
schema, mutation path, or security model. Before the Git migration slice begins, the required
forthcoming child proposal `git-core-contract.md` must define those contracts, non-Git behavior,
workspace/memory seams, compatibility, security, and executable acceptance fixtures. At minimum,
repository-derived records must be scoped to the vault/execution-policy principal authorizing
ingest, bind signed producer and repository provenance, and be redacted for secrets before
persistence. The child must include mechanical acceptance checks for those three properties. Until
it is accepted, no proposal may introduce a functional Git implementation acceptance target or
begin the Git migration slice. Taxonomy-only classification and inventory checks are explicitly
allowed before the child. The migration slice begins with the first change that adds Git module
source, registers `git` in a descriptor, build artifact, or generated profile, or reports Git
readiness.
The child may narrow responsibilities but may not make `git` optional or move code intelligence
out of `memory` without amending this suite-level decision.

Optional targets never introduce a core dependency. Routing declares a generic typed target-kind
contract; the optional `workflows` module registers the `workflow` target kind when selected. With
it omitted, workflow selection returns typed `capability_absent`, and no workflow header or symbol
is present in core.

`module-runtime` bootstraps from one generated, immutable root descriptor whose digest is embedded
in the profile. It then resolves the ordinary descriptor graph. Startup fails unless the declared,
selected, registered, and ready required-module sets are exactly equal; the bootstrap root cannot
declare feature capabilities.

## Executable core proof

The `core` profile contains no optional objects or capabilities. One MCP fixture and one ACP fixture
must each traverse these named stages in order. This proposal owns the stage boundary and order;
the memory/learning proposal owns the semantics and quality gates of stages 6, 8, 10–12, and 20.

1. module catalog resolution
2. effective-config resolution
3. gateway ingress
4. protocol decode
5. request IR shaping
6. structured extraction/indexing
7. memory write
8. embedding
9. candidate retrieval
10. reranking
11. skill context
12. learning observation
13. route selection
14. execution authorization
15. vault credential resolution
16. delegate invocation
17. workspace access
18. tool dispatch
19. audit append
20. response composition
21. response IR shaping
22. translation encode
23. gateway delivery

Every stage records its component ID and input/output IDs. Required-provider failure is typed and
fails readiness or the owning stage; silent passthrough is forbidden. The proof requires MCP and
ACP coverage, response composition, and absence of `kb-synthesis` and every other optional module.

**Bus-crossing proof (2026-07-23 amendment).** Every stage owned by a module — including the `memory`
stages (structured extraction/indexing, memory write, embedding, candidate retrieval, reranking) and
the learning, routing, delegate, tool, workspace, skills, and response-composition stages — is
dispatched as a typed event across the core↔module boundary over the shared-memory event bus, not as
an in-process call. The trace records, per stage, the event kind, the publishing and serving module,
and the trust-kernel verdict/record for the hop; a stage that reaches a module by any path other than
an authorized bus event fails the proof. The `memory` stages additionally assert the performance
budget (suite invariant 15): the shared-memory crossing stays within budget (it costs more than the
former monolithic in-process call, kept small by batching/streaming), and their events are
observed/recorded without a synchronous governance verdict on the hot path.
Core descriptors declare their stages. The generator emits the stage manifest from those
declarations and validates it against the separate canonical stage-name registry. Startup and CI
require exact equality among descriptor stages, the generated manifest, registered runtime stages,
and observed trace stages; there can be no orphan or undeclared stage.

Stages 11 and 12 make learning and skills load-bearing rather than ceremonial. The fixture begins
with a provenance-backed user correction and a matching procedural skill, requires both to affect
route/context composition, then records the outcome through learning. A missing or no-op learning
or skills provider fails the expected context, route, and recorded-outcome assertions. This defines
the fundamental Aimee round trip as adaptive and procedural, consistent with Aimee's user-learning
mission.

## Non-goals

- Making every built-in behavior core.
- Requiring dynamic shared libraries.
- Treating replaceable implementations as removable contracts.
- Defining memory inference semantics or product/web ownership; their child proposals own those
  details.
- Defining descriptors, generated builds, source migration, or compatibility policy.
- Requiring federated identity, organizational governance, policy authoring/distribution,
  fleet governance, or compliance/attestation surfaces. Optional `governance` provides those over
  the required enforcement, identity-handle, vault, and audit contracts. Binding check 5 proves
  that core does not require governance and that governance's positive ownership comes only from
  the module catalog's governance ownership contract.

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_module_deps.sh --profile core --no-cycles --no-core-to-optional --no-private-cross-imports --public-symbol-graph --link-graph && scripts/check_module_runtime_bootstrap.sh --immutable-generated-root --forbid-root-feature-capabilities --require-declared-selected-registered-ready-equality"}
- {id: 2, tier: integration, check: "make -C src test-core-contracts test-module-runtime test-config test-ir test-translation test-protocols test-gateway test-memory-code test-learning test-routing test-delegates test-tools test-workspace test-skills test-response-composition test-vault test-execution-policy test-audit"}
- {id: 3, tier: integration, check: "scripts/test_core_round_trip.sh --profile core --stage-registry src/generated/core-stage-registry.yaml --generated-stage-manifest tests/core_round_trip/core-stages.yaml --require-descriptor-manifest-registry-runtime-trace-equality --protocols mcp,acp --require-adaptive-skill-affects-context-route --require-learning-records-outcome --fail-noop-learning-skills --require-response-composition-present --require-kb-synthesis-absent --require-no-optional-link-closure --typed-provider-failures"}
- {id: 4, tier: mechanical, check: "scripts/test_module_profiles.sh --profiles core --make-cmake-object-equality --require-reference-providers --require-ready --require-production-provider-provenance --forbid-test-fixture-objects-handles-descriptors"}
- {id: 5, tier: mechanical, check: "scripts/check_optional_contract_boundary.sh --optional governance --profile core --forbid-core-requires --ownership-contract tests/baselines/modules/governance-ownership.yaml --require-catalog-owner-equality --forbid-core-capability-shadow"}
- {id: 6, tier: mechanical, check: "scripts/check_core_carving.sh --communication-core module-runtime,config,ir,translation,protocols,gateway --required-modules memory,learning,routing,delegates,tools,workspace,git,skills,response-composition --trust-kernel-in-c vault,execution-policy,audit --require-c-for-communication-core --require-separate-program-for-modules --no-core-to-module-link --no-cgo --memory-is-sink"}
- {id: 7, tier: integration, check: "scripts/test_core_round_trip.sh --profile core --require-module-stages-cross-bus memory,learning,routing,delegates,tools,workspace,skills,response-composition --trace-records-event-kind-publisher-server-verdict --fail-non-bus-module-path --memory-stages-perf-within-budget --memory-stages-async-record-no-synchronous-verdict"}
```
