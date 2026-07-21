# Core modularization slice 8: plugin-loader physical ownership

## Decision

This slice resumes physical source cleanup. It moves only the loader-specific discovery source and
header into the canonical `plugin-loader` directory. No file motion or reclassification of
`plugin.c`, `plugin.h`, `plugin_ctx.c`, `plugin_ctx.h`, `plugin_c_hook.c`, or `plugin_c_hook.h` occurs
in this slice.

The new physical owner does not imply current optionality. Both supported build graphs continue to
link the discovery source unconditionally: Make through `CORE_SRCS` and CMake through its analogous
source list. The canonical taxonomy's optional, default-disabled state remains the target reached
after the contract split and profile proof.

## Exact ownership

- `src/plugin_loader.c` moved to `src/modules/plugin-loader/plugin_loader.c`.
- `src/headers/plugin_loader.h` moved to
  `src/modules/plugin-loader/include/aimee/plugin-loader/plugin_loader.h`.
- The implementation, Runtime startup, and focused test include
  `aimee/plugin-loader/plugin_loader.h`.
- The relocated header retains `#include "plugin.h"` as explicitly expiring transition debt.

## Local verification

Each command below is recorded after execution as `pass`, `fail`, or `validation-pending`; an
unexecuted command is never treated as satisfied.

- `python3 -I -S scripts/check_plugin_loader_ownership.py`: pass
- `python3 -I scripts/tests/test_check_plugin_loader_ownership.py -v`: pass
- `make -C src build/obj/tests/unit-test-plugin-loader -j2`: pass
- `./src/build/obj/tests/unit-test-plugin-loader`: pass
- `make -C src lint`: pass; the pre-existing tier-dependency script reported missing `rg` but its
  existing fallback result remained pass
- `make -C src all -j2`: pass

## CI-authoritative gates

- `cmake -S . -B <outside-temp> -DBUILD_TESTING=ON` followed by a focused build is
  validation-pending because `cmake` is not installed in the local environment. CI is the
  authoritative CMake gate; this entry is not a local verification pass.

## Deferred to the next slice

No file motion or reclassification in this slice applies to this deferred work. The next slice:

1. classifies every exported type and function in `plugin.c`, `plugin.h`, `plugin_ctx.c`,
   `plugin_ctx.h`, `plugin_c_hook.c`, and `plugin_c_hook.h` as required contract, optional loader, or
   compatibility surface;
2. moves required extension ABI and registries into `module-runtime`;
3. separately moves the required pre-LLM hook contract into `module-runtime`;
4. leaves manifest discovery, install, enable/disable, remove, and dynamic loading under
   `plugin-loader`;
5. migrates Runtime, protocol, dashboard, CLI, and agent consumers to required registry contracts;
6. proves after each move, and once combined, that required link closure excludes plugin-loader.

## Explicitly out of scope

Descriptor-v2 OIDC, SSHSIG, external check publishing, and signed module-document attestation are
governance hardening. This slice invokes none of them and they do not gate physical source ownership
or ordinary module documentation.

## Roundtable

The design was approved after narrowing the original four-file-family move to the loader-only pair.
The initial reviews prevented mixed required ABI from being relabeled as optional implementation.
Final exact-diff approval is recorded in the pull request after verification.
