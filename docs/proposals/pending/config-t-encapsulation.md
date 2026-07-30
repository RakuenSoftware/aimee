# Proposal: `config_t` is a config-module secret

- **State:** in progress (phase A started, 2026-07-30).
- **Rule (from the owner, absolute):** if something needs knowledge from config's headers about
  `config_t`, it must be rewritten. Only config handles env vars. Only config provides
  getters/setters/accessors for settings. Narrow, documented exceptions only.

## Why this is not merely tidiness

`gen_config_accessors.py` already states the intent — "config_t is ~750 KB and its shape is a
config-module implementation detail" — and the accessor surface was generated. The call sites were
never converted, so the encapsulation is aspirational.

**Converting naively is unsafe.** Found while converting the first module: `config_field_read`
copied the value only when `config_load` returned 0, so every generated accessor returned its zero
seed on failure. That INVERTS every default-ON dial. Converting `subagent_ban_enabled` to its
accessor — the exact mechanical change this proposal asks for — turned a fail-closed guard
fail-OPEN precisely when config is broken.

Fixed first (`config.c`, `config_field_read`): copy regardless of `rc`, because `config_load_file`
runs `config_set_defaults` before it can return an error, so the declared default is present and is
the only honest answer for a read that failed. Pinned by `test_config_accessors.c`.

**Reproducing that failure needs care.** A missing config file does NOT exercise it —
`config_load_file` returns 0 ("defaults are fine"). The reachable path is strict mode + a
validation error, which returns -1 with defaults applied and field parsing not yet reached. A first
attempt at the test passed with the bug reinstated and proved nothing.

## Baseline

`scripts/check-config-encapsulation.py` **already existed on this branch** and is the
authoritative counter — a ratchet with a plant test proving a pointer-only leak is caught and
named. Use it; do not hand-grep (my initial greps undercounted, missing `.c`-side mentions).

After the phase-A work below: **253** files, **902** `config_t` mentions, **462** `config_load()`
calls. Its docstring carries the load-bearing rationale — `sizeof(config_t)` is ~750 KB, those
locals nest, and a measured chain had consumed ~6 MB of an 8 MB stack, with one added field
segfaulting `unit-test-memory-advanced` inside `config_load_file`. This is a correctness problem,
not a style preference.

Workflow: migrate a file, `--update-baseline`, commit. The check FAILS on a stale baseline too, so
a migration cannot be left un-banked.

## Phases

**A. Cross-module APIs that take `config_t` in their signature.** These block their callers: no
amount of call-site editing removes the leak. Convert the function to read via accessors and drop
the parameter. *(in progress — 78 -> 75)*

**B. Leaf call sites.** `config_t cfg; config_load(&cfg); ... cfg.field` -> `config_field()`.
Mechanical, and strictly cheaper than a 750 KB copy. Blocked behind A wherever a site feeds a
phase-A function.

**C. Accessor-surface safety.** The `config_field_read` default fix landed; the rest of the audit
is still open — see "Known hazards".

**D. Enforcement.** *Already done* — `scripts/check-config-encapsulation.py` exists and holds the
ratchet. Nothing to build; just keep the baseline current.

## Conversion pattern (established in phase A)

1. Confirm the accessors exist (`config_accessors.h`); the generator already emits one per field,
   including indexed ones for arrays (`config_workspaces(i)`).
2. Rewrite the implementation to call accessors; drop the `config_t` parameter.
3. Drop `#include "config.h"` from the header — the point is that callers stop seeing the type.
4. Fix whatever the include removal exposes. Removing it reveals headers that were never
   self-contained (`agent_types.h` reached `MAX_PATH_LEN` only through `config.h`); fix those
   properly rather than restoring the include.
5. Build + run the config and owning-module suites.

### Thread-local string buffers

`const char *config_foo(void)` returns a buffer valid until the next call to the SAME accessor on
that thread. Two different accessors can be held live at once (relied on in the workspace loop in
`guardrails_orchestrator.c`); two calls to one accessor cannot. Copy to retain.

## Done in phase A

- `computer_use_policy_from_config(const config_t *, ...)` -> `(computer_use_policy_t *out)`;
  3 call sites (guardrails, execution-policy, agent_tools). Dropped `config.h` from
  `headers/computer_use.h`.
- `kb_tsr_endpoint(const config_t *)` / `kb_ocr_endpoint(const config_t *)` **deleted**. These
  resolved config-then-env, so they were both leaks at once. Replaced by `config_tsr_endpoint()` /
  `config_ocr_endpoint()` in the config module; the KB no longer reads `AIMEE_TSR_URL` /
  `AIMEE_OCR_URL`. Dropped `config.h` from both sidecar headers.
- `config_antipatterns_bypass()` added to the config module, removing the last `getenv` from
  `guardrails_orchestrator.c` (and fixing the presence-vs-value bug — see the audit proposal).
- `guardrails_orchestrator.c` leaf sites: both workspace loops, both `skills_dispatch_advisory`
  sites, `subagent_ban_enabled`.
- Generator: `config_accessors.h` now includes `<stdint.h>` (it used `int64_t` and only compiled
  because `config.h` came first), and its banner no longer claims accessors "return 0 when no
  config can be read (fail closed)" — that sentence described the bug.
- `kb_detect_observe`, `kb_demote_run`, `kb_ranker_rerank`, `kb_ranker_rerank_with_sketch`,
  `kb_planner_search`, `kb_planner_validate`, `kb_bandit_sample`, `kb_bandit_reward` — all lost
  their `config_t` parameter. `kb_ranker`'s was pure dead weight (`(void)cfg;`).
  `kb_detect.h`, `kb_demote.h`, `kb_ranker.h`, `kb_planner.h`, `kb_bandit.h` no longer include
  `config.h` at all.
- Five now-dead `config_t` locals deleted (`kb_service_agent.c`, `kb_intel_payload.c` x2,
  `kb_service_memory.c`, plus the guardrails ones) — each was a ~750 KB stack frame kept alive
  only to feed a parameter that no longer exists. `kb_service_memory.c` carried a comment saying
  it was materialised *only* because `kb_bandit_sample` took a `config_t`; that is now resolved.

**Ratchet:** 902 -> 877 mentions, 462 -> 458 `config_load()`, 253 -> 242 files.

### Converting a function that tests drive with a hand-built `config_t`

Several tests did `config_t cfg; memset(...); cfg.some_command = ""` and passed it in to exercise a
"disabled" path. Once the function reads the LIVE config, such a test silently starts reading the
developer's real `aimee.yaml` and fails wherever that key happens to be set. Pin it instead: point
`HOME` at a fresh empty dir, `unsetenv("AIMEE_HOME")`, set `AIMEE_NO_CACHE=1`, call, restore.
`test_planner.c` / `test_bandit.c` have the helper pair to copy.

Watch for `mkdtemp` on a `static char[]` template — it rewrites the `XXXXXX` in place, so a second
call fails. Re-`snprintf` the template each time.

## Known hazards (phase C)

- **Fail-open on read failure** — fixed for scalars via defaults. Audit whether any *string*
  accessor has a caller treating `""` as a meaningful value rather than "unset".
- **`aimee.h:161` embeds `config_t *cfg` in a struct**, not just a signature. Needs its own
  decision: hold an opaque handle, or drop the member and have consumers call accessors.
- **`memory_core_internal.h:33` `memory_config_load_heap()`** mallocs a whole `config_t` inside a
  non-config module — a deliberate whole-struct copy that phase B must replace, not just rewrap.
- **Hot paths**: a few sites load once and read many fields. Per-field accessor calls are cheap
  (one pinned read each), but check the loop-heavy ones rather than assuming.

## Sequencing note

Do NOT mix phase A/B signature-and-call-site churn with the config-key additions in
`config-single-source-of-truth-audit.md`. Refactor and behavior change land separately; the audit's
new keys should be added *after* the surface they will be read through is trustworthy.
