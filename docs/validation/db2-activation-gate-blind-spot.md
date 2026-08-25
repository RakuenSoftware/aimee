# The DB2 activation gate cannot see every caller

Record of an audit of `check_db2_activation.py` and the consumer inventory it
rests on, taken while finishing the DB2-as-a-module work. The gate is sound in
its logic and wrong in its inputs: it clears activation using a caller list that
is built from `#include` spellings, and a substantial set of callers reach DB2
without a spelling it recognises.

Reproduce with the commands in each section below, from the repository root.

## Why this matters

`docs/proposals/pending/db2-as-a-go-module.md` §4.3 makes activation one atomic
ownership switch, and is explicit that **"there is no runtime switch back to
local calls"**. After the switch, no production binary other than
`aimee-module-db2` may apply DB2 SQL or link libpq.

`check_db2_activation.py` is what decides that switch is safe. It refuses while
any reviewed wire operation still has a direct production caller. A caller it
cannot see is therefore a caller that survives the cutover -- as a link failure
if DB2 objects leave the binary, or, worse, as a second in-process owner of the
database if they do not.

## How the caller list is built

`check_db2_source_boundary.py::_consumers` walks `src/`, and a file becomes a
consumer only if one of its `#include` lines satisfies `_is_db2_include`:

```python
DB2_BASENAME = re.compile(r"db2[^/]*\.h$")
...
return "db2" in parts[:-1] or bool(DB2_BASENAME.search(path.name))
```

So an include counts when the path has a `db2` directory component, or the
basename begins with `db2`. `gen_db2_declaration_ledger.py::consumer_index`
then scans **only those files** for symbol uses, and
`check_db2_activation.py` reads the result.

Every DB2 header whose basename does not begin with `db2`, included by its bare
name, is invisible to all three.

## The blind spot, measured

138 headers under `src/modules/db2/c/` have a basename that exists nowhere else
in `src/`, so an unqualified include of one resolves into the module and can
mean nothing else. 71 **production** files (110 including tests) include one of
59 such headers by bare name:

| including file | header, unqualified |
| --- | --- |
| `src/headers/aimee.h` | `agent_outcomes.h`, `feedback.h`, `notes.h`, `rules.h`, `tasks.h` |
| `src/headers/memory.h` | `anti_patterns.h` |
| `src/headers/render.h` | `decision_log.h` |
| `src/headers/kb_insights_util.h` | `org_spend.h` |
| `src/headers/kb_mgmt_token_authority.h` | `org_vault_key_use.h` |
| `src/cmd_data.c`, `src/cmd_doctor.c`, `src/cmd_onboard.c`, `src/dashboard.c`, … | `lifecycle.h` |

`src/headers/aimee.h` is the one that generalises: it is included by most of the
tree, so any file that includes it can call five DB2 headers' declarations while
showing no DB2 include of its own.

## What it costs today

Comparing the ledger's consumer list against a scan of every `.c` file outside
`src/modules/db2`:

- **326** files outside DB2 call a declared DB2 symbol.
- **283** are tracked. **43 are not**, reaching **180** distinct declarations.
- **19** of the untracked are production files, not tests.

One of them reaches a **reviewed wire operation**, which is the case the gate
exists to prevent:

```
src/modules/kb_client/kb_client_tool_registry.c:36   if (db2_is_initialized())
src/modules/kb_client/kb_client_tool_registry.c:114  if (db2_is_initialized())
```

`db2_is_initialized` is reviewed `wire-operation`, its ledger entry lists 15
consumers, and `kb_client_tool_registry.c` is not among them. The file reaches
the declaration through `#include "lifecycle.h"` and `#include "tool_registry.h"`,
both of which resolve to `src/modules/db2/c/`, and it links into the shipped
`aimee` binary (`src/Makefile:509`, `:616`).

So `check_db2_activation` reports this operation clear of direct production
callers while a shipped binary calls it directly.

A second candidate, `src/modules/benchmarks/agent_eval_memory_support.c`,
appears in a naive symbol scan but is a **false positive**: both occurrences of
`db2_embedding_dim()` are inside a comment at lines 1260-1261. It is recorded
here because the same scan will surface it again.

## Why this cannot simply be fixed in place

`check_db2_source_boundary.py` already has `_resolve_project_header`, which
resolves an include spelling to a repository path, so teaching `_consumers` to
count any include that RESOLVES under `src/modules/db2/` is a small change.

The cost is not the change, it is the baseline. `enforce_shrink_only` compares
each consumer/include pair against the seeded manifest and fails on anything
new:

```
fail("baseline-growth", f"new allowlist entry {key[1]!r} in {key[0]}")
```

Broadening discovery adds ~71 production entries at once. Every one is a
pre-existing edge being seen for the first time rather than a new dependency,
but the guard cannot tell those apart, so the change requires reseeding the
manifest -- which is precisely the reviewed act the guard is there to force.

## The options, as they stand

1. **Reseed with resolved includes.** Change `_consumers` to resolve, then
   regenerate the manifest as a schema bump the way v1 to v2 was done. Honest
   and complete; the diff is large and must be reviewed as a boundary statement,
   not as noise.
2. **Fix the spellings.** Qualify or remove the 59 unqualified includes so the
   existing rule sees them. `src/headers/aimee.h` alone would recover most of the
   reach. This shrinks the boundary rather than re-describing it, and is the only
   option that makes the tree honest instead of better-documented.
3. **Freeze the hole.** Add a check that fails when a file not on a seeded,
   shrink-only allowlist calls a declared DB2 symbol. Stops the gap widening
   without closing it, and keeps lint green today.

(1) and (3) record the problem. (2) removes it. They are not exclusive: (3)
prevents regression while (2) proceeds.

## Not addressed here

Whether the 1166 unreviewed declarations contain further wire operations. That
review is a separate gate on activation (`declarations_complete`), and its
input is this same consumer list -- so it should be redone after the list is
correct, not before.
