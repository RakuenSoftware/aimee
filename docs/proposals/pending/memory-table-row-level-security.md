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

## Decisions (settled)

1. **A workspace is a collection of projects, and it may be owned by any entity
   kind** — a principal, a team, an org, or a kind not yet defined. The owner is
   therefore **polymorphic** `(owner_kind, owner_id)`, not a foreign key into one
   table. `owner_kind` is deliberately unconstrained; the resolver fails closed on
   a kind it cannot resolve, so an unknown kind is unreadable rather than wrongly
   readable.
2. **A workspace-less memory resolves to the global scope** — readable by any
   authenticated principal. This is what makes the migration safe: existing
   untagged rows stay visible rather than disappearing when policies go live.

Consequences worth stating:

- **`org` cannot be resolved yet.** There is no org membership table in this
  schema — the `org_*` tables are budget, model, telemetry and vault records,
  none of which record who belongs to an org. An org-owned workspace resolves to
  unreadable until such a source exists. Known gap, not an oversight.
- **An unmapped workspace fails closed.** A memory tagged with a workspace that
  has no owner row is unreadable. Step 2's migration must therefore populate
  owners for every workspace in use *before* step 5, and step 4's report-only
  pass is what surfaces the stragglers.

## Correction: the conversion surface is far smaller than 365 call sites

The earlier count (267 `db2_*` entry points, 365 call sites) measures db2 *usage*,
not the number of places a tenant scope must be opened. Memory operations already
funnel their workspace/project context through a thread-local choke point,
`db2_memory_scope_context_set()` (`src/db2/memory_scope_query.c`), and it has
**two production callers**: `src/kb/kb_service_memory.c` and
`src/modules/memory/memory_core_search_c.c`. Everything else is tests.

The tenant GUC should be set per request/transaction, the way
`db2_tenant_scope_begin()` already does for the control plane, rather than at each
of 365 call sites. Step 3 is scoped to the request entry points plus that choke
point.

Also corrected: `memory_workspaces` is **legacy**. `memory_scopes`
(`scope_type='workspace'`) is the canonical scope tag, so policies must predicate
on `memory_scopes`, not the legacy table.

## Sequenced path

Each step is independently shippable and observable; none of them is "add
policies".

1. **Decide the ownership model.** Does a memory belong to a principal, a team,
   or a workspace promoted to a tenancy key? Blocks everything below. Needs an
   explicit decision recorded here.
2. **Add the tenant column + backfill.** Additive `ALTER TABLE`, a backfill for
   existing rows, and a documented answer for rows with no determinable owner.
   No behaviour change yet.
3. **Thread `db2_tenant_scope_begin()` through the memory paths.** Convert reads
   and writes to run inside a tenant scope. Still no RLS — verify by asserting
   the GUC is set, so a missed path is caught before it can fail closed.
4. **Add policies in report-only form.** Create the policies and verify their
   predicates against live traffic *without* `FORCE`, so a wrong predicate is
   visible as a diff rather than as an outage.
5. **Enable + FORCE.** Only once step 3 shows no unscoped memory path remains.
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
