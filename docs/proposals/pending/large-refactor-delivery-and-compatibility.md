# Proposal: deliver the modular refactor safely and measurably

- **State:** PENDING — roundtable-approved 2026-07-20; awaiting project acceptance
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Owns:** implementation sequence, public-surface baselines, cleanup ledgers, compatibility,
  migrations, recovery, and suite-completion profiles
- **Implementation dependencies:** all architectural child proposals
- **Date:** 2026-07-20

## Decision

Execute the suite as a sequence of ownership-family changes with frozen public-surface baselines,
complete deletion dispositions, append-only data migration, tested recovery, and measurable DRY
cleanup. This proposal coordinates implementation; it does not redefine module ownership.

## Sequence

0. Inventory feature clusters, approve dispositions, and delete the background curator
   (liveness/curator proposal).
1. Freeze dependency, surface, configuration, package, ABI, and database baselines (this proposal).
2. Land descriptor schema, deterministic Make/CMake generation, dependency enforcement, module-doc
   gates, and temporary forwarding/allowlist machinery (module/build proposal).
3. Establish all required module contracts and reference providers before moving their callers
   (core-contract proposal).
4. Move code intelligence and required inference into memory; establish learning, skills, response
   composition, and the optional KB-synthesis boundary (memory/learning proposal).
5. Move persistence/service code by memory ownership, not DB/process history (memory/learning and
   module/build proposals).
6. Extract protocol, gateway, routing, translation, delegate, tool, workspace, vault, policy, audit,
   configuration, and module-runtime implementations from composition hosts (core-contract and
   module/build proposals).
7. Rename Runtime and Control Plane, then extract their independent web modules and truthful config
   projections behind compatibility aliases (product/web/config proposal).
8. Move optional feature families, delete superseded paths, collapse global headers/build buckets,
   and prove all profiles and full-minus-one variants (this proposal, consuming every child).

The numbers express prerequisites, not a demand for strictly serial work: successors may run in
parallel once their named predecessors are satisfied. Each step has a separately reviewable PR or
small PR series. One ownership family moves per PR unless a shared seam makes separation impossible.
Contracts land before callers; path-only moves are used only for already-single-purpose code.
Blast-radius evidence and module docs update in the same PR.

## Compatibility and recovery

Baseline CLI help, routes, IR fixtures, database schemas/migrations, public
headers/symbols, plugin ABI, packages, and install manifests with narrowly defined normalization.
The product/config proposal exclusively owns configuration-surface baselines and the distinction
between advertised settings and persisted legacy input.

Compatibility records name owner and independent reviewer, affected surfaces/data, complete touch
set, effective/minimum releases, pre-change image digest and immutable tag, migration fixtures,
backup/export, forward-fix, restore and verification commands, retention expiry, and rollback
owner. Release engineering owns the deprecation clock. Retain aliases, forwarding headers, accepted
legacy inputs, images, fixtures, and recovery instructions for the longer of two stable releases or
180 days; expiry requires zero current production consumers and passing old-to-new migration
fixtures.

Every alias emits a typed usage metric and an owner alert without logging secrets. CI fails an
expired record even when consumers remain; continued use requires an independently approved new
record, never silent extension. Static analysis proves aliases contain only declared value
translation and one call to the canonical public entrypoint.

The parent proposal defines a compatibility alias. In this program aliases may accept an old CLI,
route, config, package, header, symbol, or product identifier and forward it to the canonical owner;
they may not contain a second implementation. Forwarding/allowlist machinery is the temporary
build/runtime infrastructure that enforces those recorded aliases and their expiry.

Migration fixtures include oldest-supported, immediately-prior, current-empty, and current-with-
data databases. They preserve tenant/principal identities, memory ownership, policy bindings,
vault references, audit ordering/integrity, and row/blob counts. Legacy KB policy requires explicit
tenant rebinding and reauthorization; audit projections are byte/semantically equal to authorized
deterministic views of the canonical ledger.

Migrations are append-only. Destructive data retirement is separate from source deletion and waits
for the compatibility window. `scripts/recover_refactor.sh` implements recovery for binding check 2:
it exports data, attempts a forward fix, and on failure restores the versioned backup into a fresh
database for verification with the retained prior image. It never reverses an applied migration in
place.

## Acceptance ownership

This proposal owns suite-wide surface baselines, generic disposition-ledger validation, cleanup
ledgers, database compatibility/recovery, and all-profile completion. Feature-specific children own
their own dispositions and checks; binding check 3 consumes those approved artifacts and may not
replace their stricter feature gates.

## Cleanup accounting

Every PR records production additions, deletions, consolidations, remaining fallbacks, and why net
growth is necessary. Feature-specific tests leave with deleted features. A move may not introduce
global headers, broad service locators, parallel registries, pass-through layers, or feature code in
base/composition roots. Success is fewer concepts, ownership edges, and production paths—not code
golf or directory churn. Binding check 4 accounts for production growth, consumers, and blast
radius. Reducing concepts and ownership edges is a review target recorded in each PR's cleanup
ledger, not a mechanically comparable scalar; human review judges whether a surviving abstraction
earns its complexity.
Net consolidation and net production growth are structured fields. Growth passes only with a named
consumer, rejected simpler alternative, expiry/revisit condition, and independent reviewer approval.
Every slice that can cross Runtime/Control ownership includes tenant-isolation and custody-edge
blast-radius evidence.

## Completion profiles

`core`, `runtime`, `control`, and `full` must build and pass with identical Make/CMake object sets.
Every optional module is omitted once from `full`; its objects, symbols, registration, config,
routes, assets, metrics, and background work must be absent while unrelated journeys continue to
pass.
`tests/baselines/modules/optional-absence-dimensions.yaml` is authoritative for build, link, load,
registration, config, route, asset, metric, job, telemetry, and owned-data residues. The profile
test and this generated prose list consume that artifact; neither maintains a separate flag list.

## Binding checks

```yaml acceptance
- {id: 1, tier: integration, check: "scripts/compare_surface_baseline.sh --index tests/baselines/modules/index.yaml --cli-help --routes --ir-fixtures --db-schemas --public-symbols --public-headers --plugin-abi --packages && scripts/check_compatibility_aliases.sh --forward-only --forbid-second-implementation --typed-usage-metrics-owner-alerts --fail-expired-even-if-consumed --require-independent-renewal"}
- {id: 2, tier: integration, check: "scripts/test_db_compat.sh --storage-domains personal-memory,shared-memory --matrix oldest-supported,immediately-prior,current-empty,current-with-data --append-only-migrations --tenant-principal-memory-policy-vault-audit-roundtrip --require-policy-reauthorization --pure-audit-projections --test-recovery scripts/recover_refactor.sh"}
- {id: 3, tier: mechanical, check: "scripts/check_deletion_dispositions.sh --inventories build/inventory --consume-approved-feature-dispositions --independent-approval --complete-touch-set --rollback-owner --deadlines --forbid-weaker-duplicate-feature-gates"}
- {id: 4, tier: mechanical, check: "scripts/check_cleanup_ledger.sh --require-every-slice --require-consumers --require-net-consolidation-or-structured-growth --require-rejected-simpler-alternative --require-expiry-revisit --require-independent-growth-review --require-blast-radius --require-tenant-isolation-custody-edge-evidence"}
- {id: 5, tier: integration, check: "scripts/test_module_profiles.sh --profiles core,runtime,control,full --full-minus-one-every-optional --absence-dimensions tests/baselines/modules/optional-absence-dimensions.yaml --require-generated-prose-equality --make-cmake-object-equality"}
- {id: 6, tier: mechanical, check: "scripts/check_suite_consistency.sh --index docs/proposals/pending/core-substrate-and-source-module-boundaries.md --children-required --taxonomy-equal --shared-invariants --no-duplicate-acceptance-ownership"}
```
