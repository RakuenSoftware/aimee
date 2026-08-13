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
2. **The text `project` column gets a referent.** `kb_documents.project` and friends hold a project
   NAME, not a key into `kb_project`. Either map name to `kb_project.id` or carry the id. Until that
   link exists there is nothing for a policy to test.
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

1. `kb_project_visible()` plus policies on `kb_documents` and `kb_file_index` (the search surfaces a
   user notices first). Backfill and enable together.
2. `kb_embeddings`, `kb_pdf_embeddings`, `kb_doc_regions` (the last inherits through `chunk_id`).
3. The code index: `files` through `projects`, including what `projects.workspace` means for
   attribution.
4. `ws_scope_project_path` membership check, so the filesystem and the database agree.
5. `memories`, once the question above is answered.

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

Pending. Evidence gathered on the merged state of #2632 (all counts and column facts above are read
from `src/db2/schema.sql` at that commit). Slice 5 blocked on the memory-scope decision; slices 1 to
4 blocked only on choosing a migration option.
