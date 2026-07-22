# Canonical module inventory

`canonical-inventory.yaml` is deliberately serialized as strict JSON, which is a subset of YAML
1.2. This keeps the production validator on Python's standard-library JSON decoder and prevents
YAML object construction. Do not add YAML-only syntax, comments, aliases, or tags.

Schema version 1 contains exactly 18 required IDs and 8 optional IDs. `git` is pinned as required.
Run `scripts/check_module_inventory.sh` to validate the artifact; focused failure-mode tests live in
`scripts/tests/test_check_module_inventory.py`.

`governance-ownership.yaml` is the machine-readable provider-neutral ownership projection for optional governance
plane. It is also strict JSON-compatible YAML. It deliberately duplicates no general module metadata:
module classification comes from `canonical-inventory.yaml`, and dependency edges come from
`src/modules/*/module.yaml`. The normative fenced block in
`docs/proposals/pending/module-runtime-source-ownership-and-build.md` is authoritative; the checker parses
that block and requires this artifact to match its capability map, forbidden core shadows, required
dependency list, and forbidden core-to-governance edge exactly.

Run `python3 -I -S scripts/check_capability_ownership.py` from any directory. Update the artifact only
with the proposal's normative block, then update its mutation tests in the same change. This contract
does not establish source, header, test, documentation, build, configuration, route, or data ownership;
later descriptor and profile slices own those inventories.
