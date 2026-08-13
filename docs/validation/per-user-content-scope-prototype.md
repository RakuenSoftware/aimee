# Validation: per-user content scope, slices 1 and 2 prototyped

Proves the design in `docs/proposals/pending/per-user-content-scope-visibility.md` against the real
schema on a real Postgres, including the failure it warns about.

## Environment

- **Commit**: `e569530f93` (testing, after #2632).
- **Host**: Proxmox `pvetest` at 192.168.1.252, LXC **CT 410** `aimee-ci-repro`.
- **Postgres**: 17, with `pg_trgm` and `vector` available.
- **Schema**: `src/db2/schema.sql` at that commit, applied to a scratch database `scope_probe`.
  212 tables. The file is a TEMPLATE: `halfvec(__EMBED_DIM__)` must be substituted (the migrate step
  does this at runtime), and applying it raw stops at that line with `ON_ERROR_STOP`, leaving only
  the first 111 tables and none of the tenancy ones. Substituted `768` for the probe.
- Queries run as a plain `scope_app` role, never as `postgres`: a superuser bypasses RLS even under
  `FORCE`, so a probe run as the owner proves nothing.
- The scratch database and role were dropped afterwards; `aimee_test` was not touched.

## What was built

One column, one function, one policy, exactly as the proposal describes.

```sql
ALTER TABLE projects ADD COLUMN IF NOT EXISTS kb_project BIGINT REFERENCES kb_project(id);

CREATE OR REPLACE FUNCTION kb_project_visible(p BIGINT) RETURNS boolean
LANGUAGE sql STABLE AS $$
  SELECT p IS NOT NULL AND EXISTS (
    SELECT 1 FROM kb_project k
     WHERE k.id = p
       AND k.parent IN (SELECT team FROM kb_team_membership
                        WHERE identity_key = current_setting('aimee.principal', true))
       AND (k.access_mode = 'team-open'
            OR k.id IN (SELECT project FROM kb_project_membership
                        WHERE identity_key = current_setting('aimee.principal', true))));
$$;

ALTER TABLE kb_documents ENABLE ROW LEVEL SECURITY;
ALTER TABLE kb_documents FORCE ROW LEVEL SECURITY;
CREATE POLICY p_documents_project ON kb_documents
  FOR SELECT USING (EXISTS (SELECT 1 FROM projects p
                            WHERE p.name = kb_documents.project
                              AND kb_project_visible(p.kb_project)));
```

Fixtures: teams `alpha` and `beta`; `ana` in alpha, `ben` in beta, `cara` in neither; a `team-open`
project per team, one `restricted` project in alpha, and one code-index project left unattributed.

## Result

| principal | reads |
|---|---|
| `ana`, member of alpha | `ALPHA SECRET` |
| `ben`, member of beta | `BETA SECRET` |
| `cara`, member of nothing | nothing |
| no `aimee.principal` set | nothing |

Four properties hold, each of which the proposal claims:

- **Isolation.** Neither member reads the other team's content.
- **`access_mode` is honoured.** `LOCKED SECRET` sits in a `restricted` project owned by alpha, and
  `ana` is in alpha, yet she cannot read it: she holds no `kb_project_membership` for it.
- **Deny by default.** `ORPHAN SECRET` belongs to a code-index project with no `kb_project`, and is
  invisible to everyone rather than visible to anyone.
- **Fail closed.** With no principal set, the table returns nothing.

## The failure the proposal warns about, reproduced

The obvious implementation resolves content to tenancy by NAME, because the content column already
holds a project name. Replacing the policy with that version and changing nothing else:

```sql
CREATE POLICY p_documents_project ON kb_documents
  FOR SELECT USING (EXISTS (
    SELECT 1 FROM kb_project k
     WHERE k.name = kb_documents.project
       AND k.parent IN (SELECT team FROM kb_team_membership
                        WHERE identity_key = current_setting('aimee.principal', true))));
```

with both teams owning a project called `aimee`, and a document attributed to **alpha's**:

| principal | reads |
|---|---|
| `ana`, member of alpha | `ALPHA-ONLY BOARD MINUTES` |
| `ben`, member of **beta** | `ALPHA-ONLY BOARD MINUTES` |

`ben` reads content that belongs to a team he is not in. `kb_project` is `UNIQUE(parent, name)`, so a
name identifies nothing on its own, and the join silently finds beta's row. This is a cross-tenant
leak introduced by the control meant to prevent one, and it passes any test that only checks "a
member can still see their own project".

## What this does not cover

- Only `kb_documents`. `kb_file_index`, `kb_embeddings`, `kb_pdf_embeddings` and the code index
  follow the same shape but were not exercised.
- No backfill was written; attribution here was by hand.
- `memories` has no scope column and is untouched, pending the scope decision in the proposal.
- Performance is unmeasured. The policy adds an `EXISTS` per row; whether that wants an index on
  `projects(name)` (already `UNIQUE`, so it has one) or a materialised principal-to-project set
  should be measured before enabling on a populated deployment.
