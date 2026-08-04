# Proposal: `config_t` is a config-module secret

- **State:** DONE — production encapsulation delivered and archived 2026-08-04; test/safety residual extracted.

> **Archived after partial delivery.** The ratchet now reports zero production files naming
> `config_t`; generated accessors, conversion tooling, and the lint gate hold that boundary. Test
> fixtures still construct the type and the accessor/runtime-validation hazards recorded below are
> not fully discharged. That remaining scope is
> [`config-t-test-and-accessor-safety-residual.md`](../pending/config-t-test-and-accessor-safety-residual.md).
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

### Round 3 — the memory.h cognify/episode group

Converted: `memory_cognify_unit`, `memory_cognify_drain`, `memory_episode_card_generate`.
`memory_cognify_drain` held its `cfg` purely to hand to `memory_cognify_unit`, so it fell out for
free once that one changed. Two more dead `config_t` locals deleted in `cmd_memory_vector.c`, each
a ~750 KB frame `config_load`ed to read a single boolean for an error message.

**Ratchet:** 877 -> 869 mentions, 458 -> 456 `config_load()`, 242 -> 239 files.

Six test cases in `test_memory_advanced.c` were the hand-built-`config_t` hazard above. They now
call the file's existing `write_test_config()` helper so the precondition is written to the config
file the test owns. Note this works because that binary never calls `config_snapshot_init`, so each
accessor falls through to a fresh `config_load()` from `AIMEE_HOME` — a test binary that DOES pin a
snapshot would need `config_reload()` instead. The two fixture-command cases are the useful check
that this is real: they assert `rc == 0` and match `memory_kind`, which only holds if the command
was genuinely read back out of config.

**Blocked, not done — `memory_query_rewrite`.** It reads five own fields (fine), but also passes
`cfg` to `memory_rewrite_llm_inproc`, which passes it to `kb_curator_provider_for_stage`
(`kb_curator_provider.c:116`). That has 6 call sites plus a test that hand-builds a `config_t`, so
it is its own tranche. Do the curator-provider chain first, then this falls out.
`memory_maintenance_run` / `memory_maintenance_maybe_run` also remain: two `#ifdef` variants each,
and `test_curiosity.c` + `test_memory_advanced.c` both drive them with a hand-built `config_t`.
`memory.h` is down to 4 `config_t` mentions from 7.

### Round 4 — the memory maintenance pair

`memory_maintenance_run` and `memory_maintenance_maybe_run` (plus their two `#ifdef` stub variants
and the file-static helpers `mm_interval_secs` / `mm_should_skip`) no longer take a `config_t`;
`memory_maintenance.c` names it nowhere now. One more dead `config_t` local deleted in
`kb_service_backend_memory.c` — `config_load`ed purely to feed the call.

**Ratchet:** 869 -> 858 mentions, 456 -> 455 `config_load()`, 239 -> 237 files.
`memory.h` is down to **2** `config_t` mentions from 7.

Two test hazards, both the pattern above:

- `test_curiosity.c` had **no config isolation at all** — no `AIMEE_HOME`, no temp `HOME`. It only
  survived because the `unit-tests` target exports a throwaway `HOME` for the whole run. That is the
  "none does today is luck, not a boundary" case the Makefile comment calls out. It now has its own
  `write_test_config()`.
- `test_memory_advanced.c` drove the runner with `cfg == NULL`, which the old code read as "every
  optional sub-pass off, default cadence". A null pointer cannot express that once the runner reads
  live config, so the block writes those flags off explicitly.

Both are non-vacuous: the curiosity case asserts run-then-skip, which only holds if
`interval_seconds: 3600` is actually read back out of config.

### Round 5 — memory_derive_facts, and memory.h reaches 1

`memory_derive_facts` was the `kb_ranker` pattern again: it already read its one field through
`config_memory_derive_facts_enabled()`, and kept the `config_t *` solely as a `!cfg` null guard.
Pure dead weight. No caller ever passed NULL, so dropping it changes nothing observable.
The `config_t` local feeding it in `memory_assemble.c` went with it.

**Ratchet:** 858 -> 854 mentions, 455 -> 454 `config_load()`, 237 -> 234 files.

**`memory.h` is down to 1 `config_t` mention** (from 7) — only `memory_query_rewrite`, which is
blocked behind the curator-provider chain below.

Best result of this round: `test_memory_advanced.c`'s **file-static `config_t` is deleted**. Its
comment explained it existed because ten block-scoped ~750 KiB copies in one `main()` pushed GCC
past the 8 MiB stack and segfaulted the optimized binary. With every case now writing its
precondition to a config file and the code reading it back through accessors, the suite needs no
`config_t` at all. That is the shape the whole proposal is aiming at: the workaround disappears
rather than being managed.

### Round 6 — prompts.c, and a generator hole it exposed

`prompt_apply_dispositions`, `prompt_apply_charter` and `prompt_apply_working_profile` (plus the
statics `charter_total_entries` and `working_profile_field_allowed`) no longer take a `config_t`.
**`headers/prompts.h` no longer mentions `config_t` at all** — its forward `typedef` is gone too.
One more dead `config_t` local in `cmd_session_lifecycle.c`.

**Ratchet:** 854 -> 836 mentions, 454 -> 453 `config_load()`, 234 -> 231 files.

`charter_append_section` took `const char entries[][CONFIG_CHARTER_ENTRY_LEN]` — a block of memory
the caller no longer possesses. It now takes the section's **indexed accessor** as a
`const char *(*)(int)`. Each entry is consumed inside its own `dstr_appendf`, so the accessor's
thread-local buffer is used before the next index overwrites it.

**This tranche was blocked by a bug in the generator**, fixed first in its own commit:
`gen_config_accessors.py` matched one PHYSICAL LINE at a time, so a field whose declaration
clang-format wrapped got no accessor at all.
`identity_working_profile_injection_fields` was missing its indexed accessor purely because its two
`[CONFIG_...]` dimensions did not fit on one line, while the charter arrays beside it generated
fine. Worth internalising: **the accessor surface was not as complete as it looked**, and the gap
was invisible until a conversion needed the missing accessor. Anything else that looks
"unconvertible for no reason" is worth checking against the generator before assuming the field is
special.

Test note: `test_prompts.c`'s allow-list cases needed BLOCK sequences in the written yaml —
`fields:\n  - "verbosity"`. Inline flow (`fields: ["verbosity"]`) parsed as nothing, which silently
emptied the allow list and turned it into "allow all", and the case still *looked* like it ran.
It is caught here only because the case asserts a field is FILTERED OUT.

### Round 7 — workspace.c, where the parameter turned out to be policy

`workspace_active_root`, `workspace_build_context_from_config` and
`workspace_resolve_proposal_path` no longer take or load a `config_t`;
`modules/workspace/workspace.c` and its header name it nowhere. Three more dead
`config_t` locals deleted (`agent_runtime.c`, `cmd_session_lifecycle.c`,
`server_state.c`) — two of which the compiler never warned about, because their own
`config_load(&cfg)` counted as a use.

**Ratchet:** 836 -> 824 mentions, 453 -> 449 `config_load()`, 231 -> 227 files.

**The `cfg` parameter here was not data access, it was a POLICY SWITCH.** Four callers
passed NULL deliberately. `cmd_hooks_scope.c:148` says why: resolve the project from cwd
alone so *"nested repositories and non-repository working directories cannot inherit the
configured workspace directory's name as their project label."* Dropping the parameter and
always reading config would have silently relabelled every non-repo directory sitting
inside a configured workspace. Split by intent instead of adding a boolean flag:
`workspace_active_root()` consults the configured workspaces, `workspace_active_root_from_cwd()`
never does. Five callers took the first, four the second.

Generalise: before deleting a `config_t` parameter, check whether NULL is *meaningful* at
any call site. If it is, the parameter encodes a decision and the conversion owes you two
named functions, not one.

**Latent bug fixed on the way.** In `workspace_resolve_proposal_path`, `cfg` was loaded
inside `if (config_load(&cfg) == 0)` but read again *outside* that block, so a failed load
left the second loop iterating an uninitialised `config_t`. Reading through accessors
deletes the failure mode rather than patching it.

### `make -j8` DOES NOT BUILD EVERYTHING — this bit twice

Several sources compile more than once under different defines. `workspace.c` also builds
as `build/obj/kb/modules/workspace/workspace.o` with `-DAIMEE_DB1_DISABLED`. A `(void)cfg;`
left inside an `#ifdef AIMEE_DB1_DISABLED` branch passes the default build and breaks the KB
variant — which is the EXACT defect `05abb25cb` shipped in `test_kb_http_routes.c`'s
`kb_bandit_sample` stub, reproduced here hours after fixing it.

Per-tranche check: `make all kb server`, then a full `make unit-tests`. After removing a
parameter, grep the file for `(void)<param>;` in every `#ifdef` branch.

### Round 8 — chokepoints, a converter, and what strict mode exposed

Converted by finding the functions whose signature held everything else hostage,
rather than grinding leaves:

- **`config_guardrail_mode(const config_t *)` -> `(void)`** — one field, but it kept a
  ~750 KB local alive in 7 files. Two of those locals existed for nothing else.
- **`config_embedding_command(&cfg, x)` -> `config_embedding_command_current(x)`** — 30
  call sites across 22 files. The `config_t`-free variant already existed, with a header
  note saying *"Prefer this: materialising the ~750 KB struct to read one string is what
  overflowed the stack in the memory-search path."* Converting it unblocked 11 more files
  on the next pass.
- `server_pipeline.c` (7 loads -> 0), `memory_core_helpers.c` (9 -> 0), `kb_curator_drain.c`
  (11 -> 1), `workspace.c`, `prompts.c`, the memory maintenance runner, and a 15-file
  automated sweep.

`scripts/config-convert-locals.py` does the safe shape and REFUSES the rest — missing accessor,
`(void)X;` in an `#ifdef`, `config_load` used as a guard, or the local's address escaping.
Its first escape rule matched only `func(&X` (first argument position), so
`agent_route_with_caps_scoped(&acfg, role, &route_cfg, ...)` slipped through and it deleted
locals that genuinely escape. Rule is now "any address-of other than
config_load/memset/sizeof escapes", which cut a batch from 20 files to 15.

### Converting a module changes what the LINKER needs

Four narrow test targets broke, all for the same structural reason: an accessor is opaque
where a stubbed `config_load` was transparent to LTO.

`unit-test-kb-http-ingest` is the clearest. It stubbed `config_load` to zero everything but
one flag, which let LTO prove `kb_pdf_assets_enabled` was always false, fold the branch, and
drop the call to `kb_doc_pdf_render_assets` entirely. An accessor cannot be folded, so that
branch reaches the linker and the symbol must exist.

`CONFIG_ACCESSOR_OBJS` (tests/Rules.mk) is the answer: the shards depend on nothing but
`config_field_read`, so a narrow target links them and supplies its own stub. Make the stub
read the SAME state the target's existing `config_load` stub returns — a zero-returning stub
links fine and silently makes every config value read as unset.

Verification per tranche is `make all kb server` THEN a full `make unit-tests`. Not
`make -j8`: several sources compile twice under different defines, and a `(void)cfg;` left
in an `#ifdef AIMEE_DB1_DISABLED` branch passes the default build and breaks the KB variant.

### Strict mode: what it found, and why it stayed opt-in

`g_config_strict` has no production setter, so `config_load` never returns non-zero in a
shipping binary and every `if (config_load(&cfg) != 0)` guard in the tree is unreachable —
including `server_main`'s own "server startup rejected invalid configuration".

Turning it on found **nine real keys missing from `config_schema[]`**, three of them WRITTEN
by `config_save` (aimee emitting configs it would refuse to load). Those are fixed, and
`check-config-schema-drift.py` now guards the allowlist.

It was then **reverted**, because strict treats an unknown key as an error while aimee
deliberately PRESERVES unrecognised keys (`config_set` patches the YAML in place so operator
annotations survive; `test_config_set.c` pins `custom_note: keep-me`), and `test_config.c:963`
pins that strict DOES reject them. Both contracts hold only while strict is opt-in.

**Open decision.** Making runtime validation fatal — the "config module refuses to come up"
rule — needs unknown-key (tolerate) split from known-key-wrong-shape (refuse). Until that
split exists, the 36 `config_load`-as-a-guard sites are unreachable rather than redundant,
and deleting them is still correct but for a weaker reason.

### The curator-provider chain needs a design decision first (attempted, reverted)

`kb_curator_provider_for_stage` looks like an easy six-field conversion. It is not, and the reason
generalises to any function that fills a **borrowed-pointer struct** from config.

`provider_def_t` (`provider_client.h:37`) holds `const char *base_url/model/api_key` — a borrowed
view, documented as aliasing fields inside the caller's `config_t`. Convert the function to string
accessors and those pointers now alias each accessor's `static _Thread_local` buffer instead. Every
current *production* caller consumes its def immediately, so they are fine — but
`test_kb_curator_provider.c` holds **two** defs at once (`&a` and `&b`) across ~30 assertions and
compares tier-A against tier-B routing. Under accessors both defs alias the same buffers, so those
comparisons silently stop testing anything.

The fix is not to rewrite the test to match: it is to make `provider_def_t` **own** its strings
(fixed char arrays), which removes the aliasing hazard for every caller. That changes a struct
`provider_client` and all its producers/consumers share, so it is a reviewed design change, not a
refactor tranche — and per the sequencing note it must not ride along with signature churn.

Second, smaller blocker found the same way: `config_synth_chat_endpoint(cfg, …)` is a clean
one-field conversion, but `unit-test-kb-curator-provider` links a deliberately minimal object set
(`tests/Rules.mk:5320`), and reading that field through an accessor pulls `config_field_read` →
`config.o` and its closure into a focused unit test. Decide whether these narrow test targets are
allowed to link the accessor layer before converting functions they cover.

Order to do this in: (1) make `provider_def_t` own its strings; (2) then convert
`kb_curator_provider_for_stage` and `config_synth_chat_endpoint`; (3) then `memory_query_rewrite`
falls out, since its only remaining tie to `config_t` is `memory_rewrite_llm_inproc`.

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
