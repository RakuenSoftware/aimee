# Proposal: how a module process gets cJSON

- **State:** PENDING — a decision is needed before nine DB1 sources can migrate;
  no code in this PR beyond the write-up.

Nine of DB1's remaining sources use cJSON, and none of them can be served until
the DB1 module process can link it. This is not a wire question — the frame
carries documents already — it is a question about how a self-contained module
consumes vendored third-party code, and the answer sets a precedent for every
module after this one.

## How it surfaced

`db1_trigger` was declared and cut over. Everything built except the module
binary:

    undefined reference to `cJSON_CreateObject'
    undefined reference to `cJSON_AddStringToObject'

The module already *compiles* every cJSON-using source — they are all in its
descriptor. It links today only because `--gc-sections` drops them: nothing
reachable from `main` calls one. Declaring an operation makes it reachable, and
the link fails.

So the failure is deferred, not absent. It arrives the moment any of these nine
is served:

| source | family |
| --- | --- |
| `clarify` | conversation |
| `user_memory` | conversation |
| `coord_jobs` | agent_work |
| `db1_cron_jobs` | agent_work |
| `db1_trigger` | agent_work |
| `diagnose` | telemetry |
| `ensemble` | telemetry |
| `execution_plans` | workflow |
| `mcp_osv_cache` | runtime |

Three of `agent_work`'s five sources are in that list, so the family cannot be
finished without an answer.

## Why it is not obvious

A module is deliberately self-contained: it links its descriptor's sources and
nothing else. That is the rule that lets `aimee-module-db1` build standalone,
and it is why `db1_time.c` exists to supply `now_utc` rather than the module
linking `util.c`. The same rule is what excludes `src/vendor/cJSON.c`.

For a small helper, supplying your own is right — `now_utc` is six lines and
having db1 own its copy costs nothing. cJSON is 77KB of upstream code, and the
calculus changes: a copy is a thing to patch when upstream issues a CVE, and
patching N copies is a process nobody runs N times correctly.

## The two answers

**Copy it, as DB2 already does.** `src/modules/db2/support/cjson.c` is a full
vendored copy, so the precedent exists and the change is mechanical: add
`src/modules/db1/support/cjson.c` and the header beside it. It is consistent,
it is reviewable, and it unblocks all nine immediately.

The cost is a third copy of the same library. Today that means a cJSON CVE is
patched in `src/vendor/`, in DB2's copy, and in DB1's — and the only thing
keeping those in step is that somebody remembers.

**Let a descriptor name vendored sources it does not own.** A `c_build`
field — `vendor_sources`, restricted to paths under `src/vendor/` — compiled
into the module and exempt from the ownership rule, because "owned by a module"
and "vendored third-party code compiled into a module" are genuinely different
claims and the ownership checker currently has no way to say the second.

This costs a change to `export_c_repositories.py`, the bundle builder and
`check_module_source_ownership.py`. It is the smaller diff long-term: one copy
of cJSON, and every module after this one gets the same door.

## Recommendation

The second, and DB2's copy folded into it afterwards rather than left as a
second precedent. The first is faster today and I did not take it, because a
third copy of a vendored library is the kind of decision that is cheap to make
once and expensive to unmake, and because "DB2 did it" is a weak argument for
repeating something whose cost only shows up during an incident.

That said, this is a repo-wide call about third-party code and it belongs to
whoever maintains the vendoring policy, not to the migration that happened to
hit it first. It is written down rather than decided so the nine sources are
blocked visibly instead of quietly skipped.

## What is NOT blocked

`agent_log` and `cognify_jobs` use no cJSON, so `agent_work` can advance to
those two while this is open. `cognify_jobs` has already migrated.
