# Per-user content scope: a project you cannot see returns nothing

## Problem and boundary

A user who is not a member of a project can still read that project's content.

The tenancy model is not missing. `kb_team`, `kb_project`, `kb_team_membership`,
`kb_project_membership` and `kb_admin_grant` all exist, all carry `ENABLE` plus `FORCE ROW LEVEL
SECURITY`, and `p_project_member` already states the full rule: the parent team must be one of the
principal's teams, a selected billing team narrows it further, and a `restricted` project needs an
explicit membership where a `team-open` one does not. Identity resolution is real and fails closed:
`kb_identity_resolve.c` sets `aimee.principal`, reads the principal's teams, and yields an EMPTY team
set on any lookup failure.

What is missing is that the rule stops at the governance tables. Of 204 tables, 54 are inside the
boundary. The content is outside it.

| plane | scope column | enforced |
|---|---|---|
| governance: teams, projects, memberships, budgets, entitlements, vault, token audit | `kb_project` / `kb_team` | yes |
| documents and search: `kb_documents`, `kb_file_index`, `kb_embeddings`, `kb_pdf_embeddings` | `project TEXT` | **no** |
| code index: `files` | `project_id` to `projects` | **no** |
| doc regions: `kb_doc_regions` | inherits via `chunk_id` | **no** |
| memory: `memories` | **none** | **no** |

So today a caller can be denied the `kb_project` row and still retrieve that project's documents,
embeddings, file index and code index through ordinary search. On the filesystem side the same hole
exists by a different route: `ws_scope_user_root(principal, ...)` validates the principal's name and
then returns `ws_scope_environment_root()`, the same shared root for every actor.

**Boundary.** This is about READS. Write authorisation already has its own layers (connection
capabilities, the per-user write tier, the route gate). This proposal does not revisit them.

## Decision

Content joins the boundary that governance is already inside, reusing the predicate that exists
rather than inventing a second one.

1. **One predicate, one place.** Express `p_project_member`'s rule as a function
   (`kb_project_visible(project_ref)`) and have every content policy call it. A second copy of a
   visibility rule is the defect this codebase has repeatedly shipped; the delegate permission work
   removed five copies of one role rule for the same reason.
2. **One table gains the referent, not every content table.** `kb_documents.project` holds
   `projects.name`, the CODE-INDEX project, bound at ingest (`kb_payload.c` looks up
   `projects WHERE name=?1`). `projects.name` is globally `UNIQUE`, so it identifies exactly one row.
   `kb_project.name` is `UNIQUE(parent, name)` and therefore identifies nothing on its own: two teams
   may each own a project called `aimee`. **A policy that resolved content to tenancy by NAME would
   either fail or match the wrong team's project, which is a cross-tenant leak introduced by the fix
   itself.**

   So the link belongs on `projects`, once:

   ```sql
   ALTER TABLE projects ADD COLUMN IF NOT EXISTS kb_project BIGINT REFERENCES kb_project(id);
   ```

   and every content table reaches tenancy through the name it already carries:

   ```sql
   EXISTS (SELECT 1 FROM projects p
           WHERE p.name = kb_documents.project AND kb_project_visible(p.kb_project))
   ```

   Migrations here are `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` in `src/db2/schema.sql`, applied by
   the owner migrate step, so this is one line plus a backfill.
3. **Deny is the default and the fallback.** An unattributed row is invisible. This matches
   `kb_identity_combine`, which already treats a failed lookup as an empty team set rather than a
   wider one.
4. **The filesystem answers the same question as the database.** `ws_scope_project_path` refuses a
   project the principal has no membership in, so a caller cannot reach through the workspace what
   the query layer denied.

### Non-goals

- Changing what a team or project MEANS. `access_mode` already distinguishes `team-open` from
  `restricted`, and that is the answer to "team-wide or per-user": per project, chosen by an
  operator, not by this proposal.
- Write-path authorisation.
- Any new identity source. `aimee.principal` is the identity, canonical and immutable.

## The open question this cannot answer for itself

**`memories` has no scope dimension at all.** Every other content table can be attributed; memory
cannot, because nothing records which project a memory belongs to. That is a product decision with
three defensible answers, and it should be made deliberately rather than fallen into:

- **per project.** A memory belongs to the project it was learned in, and is invisible elsewhere.
  Strongest isolation; a fact learned once must be re-learned per project.
- **per team.** Shared across a team's projects. Matches how operators describe teams.
- **per identity.** A memory belongs to whoever formed it. Strongest privacy, weakest sharing.

Until this is decided, memory is the one surface this proposal cannot close.

## Compatibility and migration

Turning on `FORCE ROW LEVEL SECURITY` over populated content tables hides every row that cannot be
attributed yet. Two honest options:

- **Backfill, then enable.** Attribute existing rows to projects, verify the counts, then enable the
  policies. No window where the control is advertised and absent. Existing single-tenant
  deployments attribute everything to the default team's project.
- **Enable with a dated escape.** A migration flag keeps unattributed rows visible while the backfill
  runs. Faster to land, and it is a hole with a date on it, which must be written down where an
  operator will read it.

Prefer the first. The second is only worth it if a deployment cannot tolerate a dark window.

## Bounded slices

1. `projects.kb_project` plus `kb_project_visible()`, additive and enforcing nothing. Safe to land
   before the backfill is proved, because no policy reads them yet.
2. Policies on `kb_documents` and `kb_file_index` (the search surfaces a user notices first),
   enabled with the backfill in the same change.
3. `kb_embeddings`, `kb_pdf_embeddings`, `kb_doc_regions` (the last inherits through `chunk_id`).
4. The code index: `files` through `projects`, including what `projects.workspace` means for
   attribution.
5. `ws_scope_project_path` membership check, so the filesystem and the database agree.
6. `memories`, once the question above is answered.

## Acceptance checks

- **Mechanical.** A principal with no membership reads zero rows from each covered table, asserted
  per table rather than in aggregate. A `team-open` project is visible to a team member who has no
  explicit project membership; a `restricted` one is not.
- **Integration.** Two identities, two projects: search, index lookup and workspace listing each
  return only the caller's project. The check that matters is the negative one, and it must fail
  when the policy is removed, because a test that passes against an unprotected table proves
  nothing.
- **Fail-closed.** With `aimee.principal` unset, every covered table returns nothing.

## Status

Pending. Amended after reading the ingest path: the content-to-tenancy link is one column on
`projects`, not a change to each content table, and resolving it by project NAME is unsafe because
`kb_project` names are unique only within a team. Evidence gathered on the merged state of #2632 (all counts and column facts above are read
from `src/db2/schema.sql` at that commit). Slice 5 blocked on the memory-scope decision; slices 1 to
4 blocked only on choosing a migration option.
