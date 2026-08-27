# Proposal: Row-level security for the memory data plane

- **State:** proposed (pending; Not started)
- **Author:** JBailes
- **Charter roles:** Constrain-Verify / Enforce, Gate-Promote

## Thesis

No `CREATE POLICY` covers any memory table. RLS today protects the **tenancy
control plane** and nothing else: `ENABLE` + `FORCE ROW LEVEL SECURITY` is
applied to exactly five tables — `kb_team`, `kb_project`, `kb_team_membership`,
`kb_project_membership`, `kb_admin_grant` (`src/db2/schema.sql`) — and all 60
policies target those plus the `org_*` billing and vault tables.

`memories`, `docs`, `memory_scopes`, `memory_episodes` and the ~20 other
`memory_*` tables have **no policy and no RLS enabled**. Every isolation
guarantee on memory rows is enforced by application SQL alone. A mis-written
JOIN or an injected predicate on a memory path has no database-layer backstop,
which is precisely the failure class RLS exists to catch — and the one the
control-plane tables are already defended against.

## Why this is not a policy-writing exercise

The obvious shape of the fix — write a predicate per table, enable FORCE, done —
**cannot be implemented as stated**, for two independent reasons.

### 1. There is no tenant key on a memory row

`memories` has no `principal`, `tenant`, `team`, or `owner` column. Scope lives
in a side table:

```sql
CREATE TABLE memory_scopes (
  memory_id BIGINT NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
  scope_type TEXT NOT NULL,
  scope_value TEXT NOT NULL,
  PRIMARY KEY (memory_id, scope_type, scope_value));
```

`scope_type` is drawn from `MEMORY_SCOPE_NONE | GLOBAL | WORKSPACE | PROJECT`
(`src/headers/memory.h:570`). That vocabulary describes **content organisation**
— which workspace or project a memory belongs to — not **tenancy**: who is
permitted to read it. Nothing in it maps to `aimee.principal` or `aimee.team`.

There is therefore no column for a predicate to reference. Writing one requires
first deciding what owns a memory (a principal? a team? a workspace?), adding
that column, and backfilling every existing row with the answer. That is a
product decision about the data model, not a schema patch.

### 2. No memory code path opens a tenant scope

The GUCs that RLS predicates read are set exclusively by `set_tenant_context()`,
reached from C through `db2_tenant_scope_begin()`. Every caller of it today is
control plane — `kb_http_*`, the vault paths, the management journals, and their
tests. **Zero files under `src/modules/memory/` open a tenant scope.**

Because the existing policies are `FORCE`d and `current_setting(..., true)`
yields NULL on an unset GUC, the design is deliberately fail-closed: no GUC means
no rows. Enabling FORCE RLS on `memories` before the memory paths set the context
would return **zero rows on every memory read** — the entire memory layer goes
dark, silently and by design.

## The surface, measured

Counted from `src/db2/schema.sql` (205 tables) and `src/modules/memory/`.

### Tables

| Class | Count | Policy strategy |
| --- | --- | --- |
| FK-reachable from `memories` | 21 | Derive ownership by joining the FK chain; one predicate shape reused |
| `memory_*` with no FK path | 8 | Each needs its own answer — see below |

The 21 reachable tables are 20 with a direct `REFERENCES memories(id)` plus
`memory_unit_edges` transitively. These are the easy ones: a policy can reach
`memories` and inherit whatever key lands there.

The 8 unreachable ones split three ways:

- **A parallel content surface — `memory_embeddings`.** This is the pgvector
  store: `point_id` (no FK to `memories`), `primary_scope`, `workspace`,
  `project`, `kind`, and `payload_json`. Memory content and metadata are
  **denormalised into it**, so a recall path that searches embeddings and reads
  `payload_json` returns content **without ever touching `memories`**. An
  FK-derived policy would not cover it. This table must be predicated
  independently or the isolation is illusory.
- **Polymorphic per-object — `memory_lineage`.** `(object_type, object_id)`
  with no FK, so it cannot be joined generically.
- **Global / singleton, no tenancy needed — `memory_active_embedder`,
  `memory_reembed_progress`, `memory_relation_schema`, `memory_health`,
  `memory_recall_shadow_deltas`, `memory_scenes`.** Config singletons, ontology
  metadata, and aggregate counters.

### Code paths

24 files under `src/modules/memory/` call db2 directly: **267 distinct `db2_*`
entry points across 365 call sites**, none of which open a tenant scope. That
is the conversion surface for step 3.

## What key is actually available

There is no identity binding anywhere in the memory data plane:

- `memories` has no `principal` / `tenant` / `team` / `owner` column.
- `workspace` and `project` are **free-form TEXT labels with no owning table**.
  There is no `workspaces` table at all; `projects.workspace` is itself just a
  label. Nothing binds either to an authenticated identity.

So both candidate models require introducing that binding from scratch:

**(a) Workspace as the tenancy axis.** Add a `workspace -> team/principal`
mapping table and predicate on it. Least invasive, because `workspace` already
exists as a column on **both** content surfaces — `memory_embeddings.workspace`
and `memory_scopes` — so one vocabulary covers the row store and the vector
store, and `memories` needs no backfill. Open sub-decision: memories with no
`memory_workspaces` row are workspace-less; the policy needs a defined answer
for them (deny, or treat as a global scope).

**(b) An owner column on `memories`.** Direct, but requires a backfill of every
existing row with an owner that the data does not currently record, and it still
leaves `memory_embeddings` uncovered because that table has no FK to join back
through — it would need the same column denormalised into it.

**Recommendation: (a).** It predicates both content surfaces on columns that
already exist, avoids inventing an owner for historical rows, and matches the
scope vocabulary the code already uses. The decision that remains is what a
workspace belongs to — a team, or a principal — and what happens to
workspace-less rows.

## Sequenced path

Each step is independently shippable and observable; none of them is "add
policies".

1. **Decide the ownership model.** Recommendation above is (a), workspace as the
   tenancy axis. What remains is what a workspace belongs to — a team or a
   principal — and what a workspace-less memory resolves to. Blocks everything
   below; record the decision here.
2. **Add the binding.** Under (a): a `workspace -> team/principal` mapping table
   and a stated rule for workspace-less rows. No `memories` backfill, and no
   behaviour change yet. Under (b) this is instead an `ALTER TABLE` plus a
   backfill of every existing row, and the same column denormalised onto
   `memory_embeddings`.
3. **Thread `db2_tenant_scope_begin()` through the memory paths.** Convert reads
   and writes to run inside a tenant scope. Still no RLS — verify by asserting
   the GUC is set, so a missed path is caught before it can fail closed.
4. **Add policies in report-only form.** Create the policies and verify their
   predicates against live traffic *without* `FORCE`, so a wrong predicate is
   visible as a diff rather than as an outage.
5. **Enable + FORCE.** Only once step 3 shows no unscoped memory path remains,
   and only with `memory_embeddings` covered by its own predicate — leaving the
   vector store open would make the whole exercise cosmetic.
6. **Extend the CI gate.** `scripts/run-p1-rls-gate.sh` is already a hard,
   non-skipping gate that provisions a real Postgres with the three-role split
   and runs `scripts/p1_rls_isolation_test.sql`. Memory-table isolation
   assertions belong there, added in the same change as step 5 so the guarantee
   cannot regress silently.

## Scope note

Until step 1 is decided, memory-row isolation remains an application-layer
property. That is worth stating plainly in any security review rather than
leaving the absence of policies to be read as an oversight: the control-plane
RLS is real and tested; the memory plane is deliberately not covered by it yet,
and copying the control-plane pattern does not transfer when what you need is
scope separation on the memory rows themselves rather than on the documents and
membership graph beside them.
