# Sixteen memory operations cannot be ported: scope does not cross the boundary

Status: blocked, needs a decision
Found: 2026-08-22, during the Go port of the db2 operation catalogue
Scan: `.db2scratch/scan_scope.py`

## The finding

Thirty C symbols behind the catalogue build their SQL with
`DB2_MEMORY_SCOPE_FILTER_SQL`. That macro reads an **ambient, process-global
scope** — workspace, project, and an include-all flag — set by
`db2_memory_scope_context_set` and stored in a thread-local.

Thirteen of the operations that reach those symbols declare `scope: "session"`
in `src/modules/db2/eventcontract/operations.json`, and their requests carry the
scope fields. Those port fine: the scope travels with the call, and the Go module
can bind it.

**Sixteen do not.** They are scope-filtered in C, declared `scope: "none"` in the
catalogue, and their requests carry no scope fields at all:

| Operation | C symbol |
| --- | --- |
| memory.briefing_active_entities | db2_memory_briefing_list_active_entities |
| memory.briefing_key_facts | db2_memory_briefing_list_key_facts |
| memory.briefing_recent_activity | db2_memory_briefing_list_recent_activity |
| memory.global_constraints | db2_memory_list_global_constraints |
| memory.kv_section | db2_memory_list_kv_section |
| memory.lifecycle_newly_superseded | db2_memory_lifecycle_list_newly_superseded |
| memory.lifecycle_stale_pending | db2_memory_lifecycle_list_stale_pending |
| memory.memory_candidates | db2_memory_list_candidates |
| memory.memory_episodes_search | db2_memory_episodes_search |
| memory.memory_key_facts_provenance | db2_memory_list_key_facts_with_provenance |
| memory.memory_search_by_pattern | db2_memory_search_by_pattern |
| memory.recall_section | db2_memory_list_recall_section |
| memory.relations_for_entity | db2_memory_relations_for_entity |
| memory.relations_search | db2_memory_relations_search |
| memory.relations_search_as_of | db2_memory_relations_search_as_of |
| memory.relations_supporting | db2_memory_relations_supporting |

## Why this is not a porting difficulty

Today these work by accident of co-location. The module adapter runs inside the
caller's process and shares its thread-local, so whatever scope the caller set
before the call is still in force during it. `module_adapter.c` is explicit about
this: for the operations that *do* carry scope it sets the thread-local, runs the
read, and then puts it back the way it found it, "because while the adapter still
runs in the caller's process it shares the same thread-local as the caller".

For these sixteen it does not set anything. It decodes a request with no scope in
it and calls straight through.

A Go module has no such thread-local to inherit. Scope would be inactive on every
call, and the filter is

```
AND (?101 = 0 OR ?102 = 1 OR (<rank>) > 0)
```

where `?101` is the active flag. **Inactive means the filter admits every row.**
So a workspace-scoped caller would receive every workspace's memories — key
facts, briefings, recall sections, relations. It would not fail, log, or look
wrong. It would return more than it should, quietly.

The same is true of the C the moment the adapter stops sharing a process with its
caller, which is what the module boundary is for. This is a defect that S4 would
expose whether or not anything is written in Go.

## What was done

Nothing was ported. Porting them faithfully is impossible — there is no ambient
scope to be faithful to — and porting them scope-free would ship the wider read.
They are simply absent from `server-go/modules/db2`, and the live-probe coverage
guard only constrains what is ported, so their absence is not papered over.

`memory.global_constraints` is where this surfaced: it was next in a batch, its
catalogue entry says `scope: "none"`, and its SQL is scope-filtered. The
catalogue's own declaration is the thing that is wrong.

## The decision needed

Three shapes, in the order I would rank them:

1. **Carry scope on the request, as the other thirteen do.** Add the scope
   fields to all sixteen request schemas, change their catalogue entries to
   `scope: "session"`, regenerate, and have the adapter set and restore the
   thread-local the way it already does for the rest. Consistent with the
   existing design and the change is mechanical, but it touches sixteen wire
   formats and every caller has to supply the scope it means.

2. **Carry scope in the envelope rather than per-operation.** One scope block on
   every invocation, applied by the module for any operation that asks for it.
   Fewer schema changes, and it stops this recurring, but it is a wire-format
   change to `db2-envelope-generic-v1` and it gives scope to operations that have
   no business with it.

3. **Decide these sixteen genuinely are global.** Then the fix is to remove the
   scope filter from their SQL, not to keep it and hope. I doubt this for the
   briefing and recall reads in particular, which exist to assemble one user's
   context, but it is the caller's intent that settles it and I cannot read that
   from the schema.

Whichever is chosen, the catalogue's `scope` field should be checked against the
C rather than trusted: it was wrong for all sixteen of these, so it is not
currently evidence of anything. `.db2scratch/scan_scope.py` does that check and
should move somewhere the gate runs it.
