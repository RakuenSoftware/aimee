# Proposal: audit feature liveness and remove the background skill curator

- **State:** DONE — curator removal delivered and archived 2026-08-04; residual audit extracted.

> **Archived after partial delivery.** The background curator's complete touch set is removed again,
> its authoritative disposition remains checked, and the default lint lane prevents reintroduction.
> The separate suite-wide inventory is now
> [`feature-liveness-inventory-residual.md`](../pending/feature-liveness-inventory-residual.md).
- **Parent:** [`core-substrate-and-source-module-boundaries-residual.md`](core-substrate-and-source-module-boundaries-residual.md)
- **Owns:** feature-liveness evidence/dispositions and deletion of the current background curator
- **Implementation dependencies:** none; this proposal may land first
- **Date:** 2026-07-20 (reconciliation note added 2026-07-23)

> **Regression update (2026-07-26).** The liveness disposition and absence checker landed, but
> `src/modules/skills/skill_curator.c`, its header, and build membership were later reintroduced in
> `e9093c70`. `python3 scripts/check_background_skill_curator_absence.py` currently exits 1 with
> `rule=deleted-file`. This proposal therefore remains live until the forbidden background worker is
> removed again and the checker is enforced in the protected lane. Foreground/operator-triggered
> skill paths remain outside the deletion scope.

> **Repair update (2026-08-04).** The reintroduced worker, config, build membership, and scheduler
> hook have been removed again. `check_background_skill_curator_absence.py` now passes and runs from
> the repository's default `make -C src lint` lane, so a future testing-branch merge cannot silently
> restore the feature island. The proposal remains pending only for the broader feature-liveness
> inventory and disposition work below.

> **2026-07-23 amendment reconciliation.** This proposal is essentially unaffected by the suite
> amendment: its liveness-evidence rule and the background-curator removal stand. The amendment only
> strengthens the evidence surface — because every inter-module interaction is now a bus event
> ([`core-substrate-and-source-module-boundaries-residual.md`](core-substrate-and-source-module-boundaries-residual.md)),
> a feature that publishes and subscribes no bus event and has no consumer outside its own cluster is
> demonstrably dead by construction, which the liveness audit may use as additional mechanical
> evidence. No change to this proposal's dispositions is required; it still may land first.

## Decision

This proposal owns the suite-wide liveness-evidence rule: before migrating a feature, prove that it
serves a supported journey or production consumer outside its own cluster. The current background
skill curator fails that rule and will be deleted as an unsafe, self-contained feature island. The
generic memory-maintenance substrate remains.

A **feature cluster** is the complete inventory unit for one capability across source, generated
code, registrations, config, surfaces, data, jobs, metrics, docs, and tests. A **delivery slice** is
the PR-bounded implementation unit defined by the delivery proposal. One cluster may require
multiple ordered slices; a slice may not partially move multiple unrelated clusters.

## Liveness evidence

Inventory feature clusters across source, generated code, registrations, config, routes/commands,
schemas/data, jobs, metrics, docs, and tests. Exclude test-only edges, self-registration,
self-scheduling, self-produced/consumed state, and config exposure from proof of utility.

Each cluster receives exactly one disposition: retain/move, consolidate, replace then delete,
delete, or time-bounded unknown; a missing disposition is **unclassified** and fails immediately.
Retention requires a named supported journey or a reachable
production consumer in another cluster, with entrypoint-to-effect evidence. Unknowns expire after
30 days; externally exposed but unproven clusters expire after 90 days. Deletion dispositions name
the complete touch set, compatibility impact, rollback owner, and independent reviewer.

## Background curator finding

The current curator does not meet the retention rule:

- it is default-off yet checks interval rather than enabled state internally;
- agent-runtime and trigger-scheduler hooks can run it concurrently despite non-idempotent state;
- an async server wrapper has no live caller;
- it scans only global skills and misses project scope;
- filename-prefix grouping and whitespace overlap are not reliable semantic evidence;
- `use_count` observes injection, not actual activation or outcome;
- protected, pinned, bundled, and user skills can appear in emitted clusters;
- the promised snapshot is a TODO and `snapshot_path` is unused;
- its DB1 maintenance plan has no production consumer, can silently truncate invalid JSON, ignores
  persistence errors, and reports unreliable metrics;
- tests exercise the island itself, not a supported user journey.

## Required deletion

The disposition at `docs/audit/dispositions/background-skill-curator.yaml` is the authoritative
touch-set inventory. It includes `skill_curator.c/.h`, curator config, both scheduling hooks, the
unused async wrapper, curator-specific metrics, its maintenance artifact, and self-only tests/stubs.
Delete the code and surfaces now. Do not delete the generic `maintenance_state` storage used by
memory. Existing curator-specific rows become inert and follow the delivery proposal's data-
retirement compatibility window rather than receiving a destructive migration in this change.

The disposition records the exact curator key/tag allowlist and its historical producers and
consumers. After deletion, CI forbids every read, write, scheduler, registration, or metric that
references those keys/tags. Generic `maintenance_state` may create only descriptor-declared keys;
new keys require an approved owner and supported journey. A behavior-shape rule also rejects a
renamed workflow/job that periodically groups skills, scores similarity/use, and proposes or writes
skill changes without the learning admission contract.

Any future replacement is separately proposed under the memory/learning proposal's admission
contract. This proposal neither authorizes nor implements it.

## Acceptance ownership

This proposal exclusively owns the curator disposition and feature-specific deletion check. The
delivery proposal owns the suite-wide disposition ledger, cleanup ledgers, compatibility windows,
and data retirement; its generic gate consumes this proposal's approved disposition.

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/audit_feature_liveness.sh --exclude-test-edges --exclude-self-registration --require-non-self-endpoint-proof --fail-unassigned --fail-unclassified --fail-expired-unknown --fail-expired-exposed --fail-retained-self-islands --require-one-disposition-per-cluster --require-touch-set --require-compat-impact --require-rollback-owner --require-independent-reviewer --require-expiration-date"}
- {id: 2, tier: mechanical, check: "scripts/check_feature_deletion.sh --feature background-skill-curator --authoritative-touch-set docs/audit/dispositions/background-skill-curator.yaml --require-historical-key-tag-producer-consumer-allowlist --forbid-curator-key-tag-readers-writers --forbid-source skill_curator.c,skill_curator.h --forbid-config skill-curator --forbid-scheduling-hooks --forbid-async-wrapper --forbid-feature-metrics --forbid-feature-maintenance-writers-readers --forbid-self-only-tests --preserve-generic-maintenance-state --require-descriptor-owned-maintenance-keys --curator-evasion-behavior-lint --preserve-inert-rows-until-compat-expiry"}
- {id: 3, tier: integration, check: "make -C src test-memory test-learning test-skills test-workflows && scripts/check_surface_absence.sh --feature background-skill-curator"}
```
