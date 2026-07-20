# Canonical module inventory

`canonical-inventory.yaml` is deliberately serialized as strict JSON, which is a subset of YAML
1.2. This keeps the production validator on Python's standard-library JSON decoder and prevents
YAML object construction. Do not add YAML-only syntax, comments, aliases, or tags.

Schema version 1 contains exactly 18 required IDs and 8 optional IDs. `git` is pinned as required.
Run `scripts/check_module_inventory.sh` to validate the artifact; focused failure-mode tests live in
`scripts/tests/test_check_module_inventory.py`.
