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
`src/modules/db2`, with comments stripped:

- **19 production files** call a declared DB2 symbol while absent from the
  consumer list. **17** of them are unguarded, so those calls are live.
- Between them they reach declarations the ledger attributes to nobody.

| untracked production file | declared DB2 symbols called |
| --- | --- |
| `src/kb/kb_mgmt_token_authority_service.c` | 13 |
| `src/kb/kb_mgmt_status_authority_main.c` | 9 |
| `src/kb/kb_mgmt_status_provision_main.c` | 6 |
| `src/modules/benchmarks/agent_eval.c` | 6 |
| `src/kb/kb_mgmt_jwks_publish_main.c`, `src/kb/kb_vault_operator_runtime.c`, `src/modules/vault/vault_reseal_orchestrator.c`, `src/modules/benchmarks/agent_eval_memory_support.c` | 5 each |
| 11 more | 1-4 each |

### The gate is not currently wrong

Exactly one untracked production file reaches a **reviewed wire operation**, and
it does not survive compilation:

```
src/modules/kb_client/kb_client_tool_registry.c:35   #if !defined(AIMEE_DB2_DISABLED)
src/modules/kb_client/kb_client_tool_registry.c:36       if (db2_is_initialized())
```

`src/Makefile:1645` gives that translation unit an explicit rule compiling it
with `-DAIMEE_DB2_DISABLED`, so the call is preprocessed away. Verified on the
built object rather than argued from the source:

```
$ nm -u build/obj/modules/kb_client/kb_client_tool_registry.o | wc -l
13
$ nm -u build/obj/modules/kb_client/kb_client_tool_registry.o | grep -c db2
0
```

**So no reviewed wire operation has a live untracked caller today**, and
`check_db2_activation` is not currently clearing something it should refuse.
An earlier revision of this record claimed otherwise; that claim was wrong and
is corrected here.

A second apparent case, `src/modules/benchmarks/agent_eval_memory_support.c`,
mentions `db2_embedding_dim()` only inside a comment at lines 1260-1261. It has
real calls to other DB2 symbols, but none to a reviewed operation.

### Where it does bite

The ledger models neither of the two things that decide whether a call is real:
an include spelled without `db2`, and a call removed by `AIMEE_DB2_DISABLED`.
It counts a tracked file's textual uses, and nothing else.

That is survivable while 41 declarations are reviewed and 1166 are not. It stops
being survivable during the review itself, because the review's evidence is this
same list. Of the 1166 unreviewed declarations, **443** appear to have no
outside production consumer at all -- 275 reachable only from tests, 168 from
nobody -- which invites the inference that they are private by construction.
That inference is unsound on this data. `db2_agent_outcome_recent_failures` is
recorded with **zero** consumers and is called at
`src/modules/benchmarks/agent_eval.c:373`, reached through
`src/headers/aimee.h`.

So the ordering matters: correcting the consumer list is a prerequisite for the
declaration review, not a cleanup that can follow it. A symbol classified
`private-db2` because the ledger showed no callers, when callers exist, is a
wire operation that will not be built -- and the cutover discovers it as a link
failure.

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
review is a separate gate on activation (`declarations_complete`), and its input
is this same consumer list -- so it must follow the correction rather than
precede it, for the reason given above.

Also not addressed: whether the 17 live untracked callers should be callers at
all. Several are operator and management entry points
(`kb_mgmt_token_authority_service.c` reaches 13 declarations) whose DB2 use may
be legitimate and simply undeclared, or may be a boundary violation that has
never been visible enough to argue about. Deciding that is the review this
record is asking for, not something to settle from a symbol count.
