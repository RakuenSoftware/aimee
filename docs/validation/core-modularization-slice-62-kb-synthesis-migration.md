# Core modularization slice 62: migrate and latch `kb-synthesis` (Class A #5, final movable)

## Scope

This slice migrates the optional `kb-synthesis` module by relocating the KB curator family, 21 sources
and 16 headers, from `src/kb/` into `src/modules/kb-synthesis/` and latching the descriptor. It is a
whole-family move, not a split. No production symbol, behaviour, storage, or configuration changes;
every curator symbol keeps its name.

## Why this one is KB-tier (self-reviewed)

The panel review of the effort flagged `kb-synthesis` as the hardest remainder: the curator is deeply
coupled to the KB. That coupling is real but it is *inward*, the curator sources include KB-internal
service headers (`kb.h`, `index.h`, `kb_service_graph.h`, `kb_service_code_embed.h`, `kb_memory_facts.h`,
`kb_mdl.h`, `kb_learning_synth.h`, `kb_evidence_embed.h`, `kb_background.h`, `kb_http_ws.h`), while the
files that consume the curator (`kb.c`, `cmd_kb.c`, the curator config/profile) call it through the
`kb_curator_*.h` headers. So the family moves as a unit; the module is **KB-tier**.

Being KB-tier is the whole build story. The curator objects must compile with the KB build flags (the
`$(OBJDIR)/kb/%.o` rule: `-DAIMEE_DB1_DISABLED`, `-DAIMEE_DISABLE_DB2_SQLITE_SHIM`, the KB include set)
and link only into `aimee-kb`, not the server. So the sources were **removed from `KB_SRCS`** and given
their own `KB_SYNTHESIS_SRCS`/`KB_SYNTHESIS_OBJS` pair whose objects resolve to
`$(OBJDIR)/kb/modules/kb-synthesis/kb_curator_*.o` (KB obj tree, KB flags), mirroring the existing
`kb/modules/benchmarks/` precedent. `KB_SYNTHESIS_OBJS` is added to the `aimee-kb`, `aimee-negation-eval`,
and `aimee-blast-radius-eval` link targets; `KB_SYNTHESIS_SRCS`/`OBJS` join `ALL_SRCS`/`ALL_OBJS` for
lint and dependency tracking.

`-Imodules/kb-synthesis` is added to the global `C_FLAGS` so every consumer, in `aimee-kb` and in the
server/CLI binaries that include the curator config/profile headers, resolves `kb_curator_*.h`. The 20
distinct curator object references across `tests/Rules.mk` (`$(OBJDIR)/kb/kb_curator_*.o`) and the seven
`CMakeLists.txt` source references were repointed to the new paths.

## What stays

`kb_curator_provider.c` (a provider adapter in `CORE_SRCS`, not KB-tier) and the DB2 artifact/link
storage APIs are not curator implementation; they stay their owners' and are consumed through their
contracts. `config_kb_curator.c` is a `config`-module file (curator configuration), not curator, and is
untouched.

## Ownership domain and tests

The module root holds exactly the 21 declared sources and 16 declared private headers;
`docs = ["docs/modules/kb-synthesis.md"]`. The latch is exact. `agent`-facing curator headers are
declared as `private_headers` per the flat-layout convention even though in-KB consumers include them.
`tests` declares the 22 `test_curator_*`/`test_kb_curator*` tests whose subject is the curator; the
test-registration baseline records them.

## Verification

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 scripts/tests/test_validate_module_descriptors.py
python3 scripts/check_module_docs.py
python3 scripts/check_module_source_ownership.py
python3 scripts/check_module_header_layout.py
python3 scripts/check_module_test_registration.py
python3 scripts/check_module_inventory.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/refactor_baselines.py check
make -C src ../aimee-server        # consumer side + global C_FLAGS (curator objects are KB-only)
make -C src ../aimee-kb            # curator compiled KB-tier (requires libp11-kit-dev)
```

`aimee-server`/`aimee` build clean locally (the consumer includes resolve). `aimee-kb` and the curator
tests require `libp11-kit-dev` and are validated on the .253 aimee-test container: the curator sources
compile KB-tier at `$(OBJDIR)/kb/modules/kb-synthesis/`, `aimee-kb` links, and the `test_curator_*`
suite passes. Self-reviewed under explicit operator authorization; the linker enforces the partition.

## Where the programme stands

Twenty-three of twenty-six descriptors are latched. The five Class A modules whose code existed (`benchmarks`, `tools`, `routing`, `execution-policy`, `kb-synthesis`) are all migrated. The three that
remain (`control-web`, `runtime-web`, `response-composition`) are **no-source** (their implementation
does not exist yet; the web GUIs are owned by `product-governance-web-and-config.md`) and are correctly
kept as valid, unlatched descriptors.
