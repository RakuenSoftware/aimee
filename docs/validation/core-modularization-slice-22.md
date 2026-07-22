# Core modularization slice 22: plugin-loader ownership

## Scope

This slice starts from `3cfff68e95674c0384883b9f6d358ff2f86db2a0` and enriches only the
optional, default-disabled `plugin-loader` descriptor. It declares two sources, two public headers,
two focused tests, and the canonical module document. It changes no schema, validator, build input,
profile, source, test implementation, or runtime behavior.

## Ownership decision

`src/tests/test_plugin.c` belongs to `plugin-loader` because its primary behavior is manifest
handling, installed-state persistence, context-backed plugin registration, and lifecycle behavior
implemented by `plugin.c`. Its module-runtime calls are dependency setup, not a second ownership
claim. `src/tests/test_plugin_c_hook.c` remains owned only by required `module-runtime`.

The other focused test, `src/tests/test_plugin_loader.c`, directly exercises discovery, precedence,
capacity, environment requirements, and project gating across both plugin-loader sources. No test
split, dual ownership, checker exception, or inferred directory claim is introduced.

## Liveness and build boundary

The existing enabled profile compiles both sources and runs both declared tests. The default-off
profile continues to prove the sources, symbols, management surfaces, and dynamic-loading behavior
are physically absent. Descriptor ownership remains documentation and validation only; Make and
CMake continue to select build inputs independently in this slice.

## Completed local verification

- `python3 -I -S scripts/validate_module_descriptors.py --check-schema src/modules`
- `python3 -I -S scripts/validate_module_descriptors.py --emit-ownership src/modules`
- `python3 -I scripts/tests/test_validate_module_descriptors.py -v`
- `make -C src AIMEE_WITH_PLUGIN_LOADER=1 build/obj/tests/unit-test-plugin build/obj/tests/unit-test-plugin-loader`
- `src/build/obj/tests/unit-test-plugin && src/build/obj/tests/unit-test-plugin-loader`
- `make -C src lint`
- `python3 -I -S scripts/check_cleanup_ledger.py`

## Close-time gates

- technical-writer review and exact-final-diff roundtable approval
- feature-branch pull-request CI, including enabled/disabled plugin-loader profiles
