# Proposal: define Aimee's required core capability contract

- **State:** PENDING — roundtable-approved 2026-07-20; awaiting project acceptance
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Owns:** required module responsibilities, core dependency law, and the executable core proof
- **Implementation dependency:** module descriptor/build enforcement
- **Date:** 2026-07-20

## Decision

Aimee Core consists of seventeen required modules. Each owns a narrow public contract and one
working reference implementation. None imports, links, loads, or requires an optional module.

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

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_module_deps.sh --profile core --no-cycles --no-core-to-optional --no-private-cross-imports --public-symbol-graph --link-graph && scripts/check_module_runtime_bootstrap.sh --immutable-generated-root --forbid-root-feature-capabilities --require-declared-selected-registered-ready-equality"}
- {id: 2, tier: integration, check: "make -C src test-core-contracts test-module-runtime test-config test-ir test-translation test-protocols test-gateway test-memory-code test-learning test-routing test-delegates test-tools test-workspace test-skills test-response-composition test-vault test-execution-policy test-audit"}
- {id: 3, tier: integration, check: "scripts/test_core_round_trip.sh --profile core --stage-registry src/generated/core-stage-registry.yaml --generated-stage-manifest tests/core_round_trip/core-stages.yaml --require-descriptor-manifest-registry-runtime-trace-equality --protocols mcp,acp --require-adaptive-skill-affects-context-route --require-learning-records-outcome --fail-noop-learning-skills --require-response-composition-present --require-kb-synthesis-absent --require-no-optional-link-closure --typed-provider-failures"}
- {id: 4, tier: mechanical, check: "scripts/test_module_profiles.sh --profiles core --make-cmake-object-equality --require-reference-providers --require-ready --require-production-provider-provenance --forbid-test-fixture-objects-handles-descriptors"}
```
