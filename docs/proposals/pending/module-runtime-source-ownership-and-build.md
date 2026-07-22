# Proposal: make module ownership drive source, builds, config, and documentation

- **State:** PENDING — roundtable-approved 2026-07-20; awaiting project acceptance
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Owns:** descriptor schema/validation, physical source ownership, generated Make/CMake and
  ownership-map outputs, include/type/symbol/link dependency enforcement, and complete individual
  module-documentation gates
- **Implementation dependency:** feature-liveness dispositions identify what should move
- **Date:** 2026-07-20

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
`src/db2/`, and `src/headers/`. Database files move by owning capability, not by database number.
Temporary forwarding headers and root allowlists have owners, expiries, and may only shrink.

## Descriptor contract

Every descriptor declares identity, kind, required/optional state, default selection, runtime
toggle support, dependencies, required components, providers, source and public-header globs,
config ownership/read evidence, routes/commands/protocols, data ownership, tests, docs, and
compatibility aliases. Invalid, duplicate, cyclic, unowned, or incomplete descriptors fail before
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

## Generated builds and dependencies

One deterministic generator emits sorted Make and CMake fragments plus object-to-module and
symbol-to-module maps. CI compares the selected source/object sets byte-for-byte across build
systems and every declared profile.

Dependency enforcement combines compiler depfiles, preprocessor line markers, public AST/type
references, generated-header producer edges, and selected-object symbol edges. Public headers may
include only declared public dependencies; private cross-module imports, undeclared edges, cycles,
and core-to-optional edges fail with file/line/symbol ownership evidence.

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
```
