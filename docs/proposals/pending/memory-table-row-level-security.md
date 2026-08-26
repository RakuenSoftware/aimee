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
