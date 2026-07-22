# Core modularization slice 13: individual module-documentation gate

## Outcome

This slice makes individual module documentation mechanically accountable without creating 24
placeholder documents. The 26 production descriptors remain the module-ID authority.
`tests/baselines/modules/documentation-status.yaml` partitions those IDs into substantive documents
and explicit debt. The first substantive family is `module-runtime` plus `plugin-loader`.

`scripts/check_module_docs.py` rejects malformed/duplicate descriptors, orphan or symlinked docs,
status drift, missing or reordered sections, ungrounded/placeholder sections, duplicate bullets,
path escapes/symlinks, and dependency/runtime-toggle claims (including boolean values) that
disagree with the descriptor. Its stable report lists every module as `PASS` or `DEBT`.
A document cannot exist while its module remains debt, so promotion requires a complete document and
an explicit status change in one review.

## Cleanup and scope

The two existing documents were consolidated into the common thirteen-section contract. Repeated
ownership prose at the runtime/plugin boundary became cross-links, while current lifecycle limits
and removal candidates remain explicit. No production source, descriptor, build graph, route,
configuration, or runtime behavior changes in this slice. The remaining 24 documents are real debt,
not generated prose or stubs.

## Verification

- `python3 -I -S scripts/check_module_docs.py`
- `python3 -I scripts/tests/test_check_module_docs.py -v`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `make -C src lint`
- feature-branch pull-request CI
