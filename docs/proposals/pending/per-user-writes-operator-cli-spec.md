# Operator CLI for per-user `/v1` write grants — command spec

Increment 5 of [per-user-remote-writes-authz](per-user-remote-writes-authz.md).
Written for review **before** implementation, because the previous roundtable
blocked this increment on the absence of exactly this document.

## CORRECTION (round 4)

The first three revisions of this document opened by asserting that a grant "can only
be created with hand-written SQL against `kb_write_tier_grant`". **That is false, and I
found it by trying to implement the spec.**

Already present and tested:

| layer | what exists |
| --- | --- |
| SQL | `kb_write_tier_grant_set(server,team,subject,tier,granted_by)` and `kb_write_tier_grant_revoke(server,team,subject)` — SECURITY DEFINER, admin-or-team-lead authorization, WORM audit via `kb_audit_worm_append`, tier validation, and an idempotent upsert that already clears `revoked_at` |
| C | `db2_write_tier_grant_set` / `_revoke` / `_lookup` / `_list` (`src/db2/write_tier_grant.c`) |
| tests | `scripts/per-user-write-tier-rls-test.sql`, exercising admin and lead authority, the team boundary, revocation retaining the row, and the audit rows |

I discovered this the worst possible way: I wrote a fresh
`kb_write_tier_grant_revoke(TEXT,BIGINT,TEXT)` with `DROP FUNCTION IF EXISTS` and the
**same signature**, which silently replaced the tested one and swapped its
authorization from admin-or-lead to owner-only. The P1 RLS gate caught it —
`per-user-write-tier-rls-test.sql:199: ERROR: grant administration denied` — and that
gate is the only reason this is a corrected document rather than a regression. All of
my duplicate SQL has been reverted.

So increment 5 is **narrower than three rounds of review believed**: the data and C
layers are done. What is missing is only the operator-facing surface — four CLI
commands over the existing `db2_write_tier_grant_*` seam, and nothing below it.

### What this changes in the sections below

1. **No new SQL, and no changes to the existing functions.** The spec composes what
   exists.
2. **`changed` / `was_revoked` / `previous_tier` are computed at the CLI layer** with a
   `db2_write_tier_grant_lookup` before the mutation. The existing `_set` returns
   `VOID`, and altering a tested function's signature to report what a caller can
   observe for itself would be gratuitous.
3. **`set` after `revoke` already clears `revoked_at` in place**, in the existing
   upsert. The long deliberation earlier in this document reached the same answer the
   shipped code had already made; it now cites that code instead of presenting it as a
   new decision.
4. **The authorization sections are the one place the CLI is deliberately STRICTER than
   the layer beneath it.** See below — that is a real decision, not an oversight, and it
   does not alter the DB rule.

## Why this exists

§7 of the proposal makes the local UDS operator the root of trust and says that
operator "configures kb (OIDC issuer profile *or* PAM + the `{subject → team, tier}`
grants)". §6 adds that after upgrading, "operators populate grants post-upgrade
(documented procedure); we do **not** auto-map the old global into a wildcard grant".

The primitives for that exist. What does not exist is any way for an operator to invoke
them: there is no `aimee` command, so the documented procedure has no tool. A deployment
taking this change lands fail-closed with zero grants, and the only route to a first
grant is a C caller nobody has written or `psql` as a role holding EXECUTE.

That is the gap, and it is a thin one. It is worth closing precisely because it is thin:
the alternative is an operator writing SQL against a security-critical table.

## Commands

All are `aimee kb ...` subcommands, matching the existing `kb` family in
`src/cli_v1_routes.c` (`kb search`, `kb status`, ...). Each needs a `/v1` method,
which the CLI route table enforces — a command without one fails
`make cli-v1-routes-check`.

| command | `/v1` method |
| --- | --- |
| `aimee kb grant set` | `kb.grant.set` |
| `aimee kb grant revoke` | `kb.grant.revoke` |
| `aimee kb grant list` | `kb.grant.list` |
| `aimee kb grant show` | `kb.grant.show` |

### `aimee kb grant set`

```
aimee kb grant set --subject <subject> --server <server_id> --team <team_id>
                   --tier off|data|full [--json]
```

Creates or updates the grant for exactly one `(server_id, team_id, subject)`.

- `--subject` — a canonical subject in one of the grammar's four forms: `owner`,
  `oidc:<iss>:<sub>`, `cert:<issuer>:<serial>`, or a bare host account. Validated
  client-side against the same predicate the schema CHECK mirrors
  (`db2_intent_canonical_actor`) so a malformed subject is refused before a round
  trip.
- `--tier` — `off`, `data` or `full`. `off` is a real tier meaning "explicitly
  denied", which is **not** the same as no grant: it records a decision, and it
  survives a later `list` so an operator can see the denial was intentional.
- **Idempotent.** Running it twice with the same tier is a no-op that still
  answers `200`. Running it with a different tier updates in place and is
  reported as `changed: true` with the previous tier echoed, so a script can tell
  "already correct" from "just widened somebody's access".

#### `set` against a revoked grant

The first draft left this undefined and a review caught it: the exclusion table said
a revoked row must not be resurrected, while `set` was specified as an idempotent
upsert on the unique `(server_id, team_id, subject)` key. Both cannot hold.

**`set` clears `revoked_at` in place and answers `200` with
`changed: true, was_revoked: true`.** There is exactly one row per triple, ever.

This is not a new decision: the shipped `kb_write_tier_grant_set` already does it, via
`ON CONFLICT ... DO UPDATE SET ..., revoked_at = NULL`. The reasoning below is why that
existing behaviour is the right one to build on, not a proposal to change it. The
`changed` / `was_revoked` / `previous_tier` fields are computed by the CLI from a
`db2_write_tier_grant_lookup` taken before the mutation.

Why in place rather than a second row:

- The uniqueness is not incidental. The intent writer reads
  `WHERE server_id=… AND team_id=… AND subject=… AND revoked_at IS NULL`. Two rows
  for one triple — one revoked, one live — makes that query's correctness depend on
  there being exactly one non-revoked row, which nothing enforces. A history table
  would need its own design; smuggling one in as a side effect of `set` is worse
  than either option.
- The audit continuity the exclusion table was protecting lives in
  `kb_audit_event`, not in the grant row. Every mutation appends there, so
  grant → revoke → grant is fully reconstructible with actor and timestamp per step.
  The grant row is current state; the audit log is history. Conflating the two was
  the error in the first draft.

`show` reports `was_revoked` when a prior revocation was cleared, so an operator can
see the authority was withdrawn and restored rather than held continuously.
`list --include-revoked` widens the listing to include revoked rows **alongside**
live ones — it is not a filter that shows revoked rows only. (An earlier draft said
both in different places; this is the definition.) Ordering is unaffected because
clearing a revocation changes a column, never the row count.

Refusals:

| condition | status |
| --- | --- |
| subject outside the grammar | `400` |
| tier not one of the three | `400` |
| team does not exist, or caller not scoped to it | `403` |
| server not in the registry for that team | `403` |
| caller is not the local UDS operator | `403` |

### `aimee kb grant revoke`

```
aimee kb grant revoke --subject <subject> --server <server_id> --team <team_id> [--json]
```

Sets `revoked_at`. **Idempotent**: revoking an already-revoked grant answers `200`
with `changed: false` rather than `404`, because the operator's intent ("this
subject must not write") is satisfied either way and a script retrying after a
timeout must not fail.

Revoking a grant that never existed answers `404` — that is a different situation
(likely a typo'd subject) and silently succeeding would let an operator believe
they had closed access they never held.

**Revocation takes effect immediately for minting, and is not retroactive to tokens
already minted.** The review asked for a ruling on this, so it is decided here
rather than left to the implementation.

`kb_management_identity_authority_snapshot` re-reads the grant under its own
`FOR SHARE` at mint time and refuses if it moved. A revoked grant therefore stops
producing tokens *at once* — no cache, no window. The revocation list the review
offered as a remedy would add nothing: the grant already is that list.

What survives is only a token already in a caller's hands, bounded by two
independent properties: a `300s` maximum lifetime, and single use (the `jti` is spent
on first use and `key_use` refuses a second). Worst case is one further write within
five minutes.

That residue is accepted, not closed. Closing it would mean the server consulting kb
on every `/v1` write, reintroducing the kb→server dial §1 of the proposal explicitly
avoids, to shorten a five-minute single-use window. A deployment needing a hard
cutoff revokes the *enrollment* — which the snapshot also re-reads — rather than
adding a second revocation path here.

`revoke` must say this rather than let an operator assume it cut off an in-flight
session:

```
revoked alice on mintsrv (team 770001)
  no further tokens will be minted for this subject on this server
  note: one token already minted may remain usable for up to 300s (single use)
```

### `aimee kb grant list`

```
aimee kb grant list [--server <id>] [--team <id>] [--subject <s>]
                    [--include-revoked] [--json]
```

Every filter is optional and they AND together. By default the listing contains only
live grants; `--include-revoked` WIDENS it to contain revoked ones as well, each with
its `revoked_at` populated. It is not "revoked only" — there is deliberately no way
to list revoked grants in isolation, because the question an operator asks is "who
can write to this server", and an answer that omitted the live rows would invite
misreading. Output is sorted by `(server_id, team_id, subject)` so a diff between two
runs is meaningful.

Table output, and `--json` yielding `{"grants":[...]}` — matching the
`"grants"` array-key convention the CLI route table already uses for
`toolset list` → `"toolsets"`.

### `aimee kb grant show`

```
aimee kb grant show --subject <s> --server <id> --team <id> [--json]
```

One grant with its full audit trail: `tier`, `granted_by`, `granted_at`,
`revoked_at`. `404` when absent. Exists separately from `list` because "what
exactly does this one subject have, and who gave it to them" is the question asked
during an incident, and grepping a table is a worse answer.

## Authorization

**The CLI surface is local UDS operator only.** The route requires `is_tcp == 0`, the
same condition `server_http_conn_caps` uses to return `CAPS_ALL`.

**This is STRICTER than the SQL beneath it, deliberately.** The existing
`kb_write_tier_grant_set` accepts an admin *or* a team lead, and that rule is not
changed — it is tested, other callers may rely on it, and tightening a shipped
authorization check as a side effect of adding a CLI would be exactly the kind of quiet
regression the correction above describes.

The asymmetry is justified rather than accidental. A remote caller reaching this CLI
would already hold a write tier on some server; if that were enough to administer
grants, anyone with `full` could widen their own access and the tier system would be
decorative. Restricting the *new* surface costs nothing, because the surface has no
existing users. If remote grant administration is wanted later it needs its own
delegation design — and it should be argued for on its own terms, not inherited by
default from a command that happened to be added.

Concretely: two independent checks must both pass. The route establishes that the
connection is local; the SQL establishes that the actor is an admin or the team's lead.
Neither is the whole rule.

## Audit

Nothing to add: the existing SQL functions already append to `kb_audit_event` through
`kb_audit_worm_append` on every mutation, with `aimee.principal` as the actor, and
`kb_write_tier_grant_revoke` audits unconditionally — an attempt to revoke a grant that
is absent or already revoked is still an operator action worth reconstructing.

`list` and `show` add no audit rows, because they are reads and auditing them would bury
the mutations. The acceptance criteria below verify the existing behaviour rather than
requiring new behaviour.

## Acceptance criteria

Each of these is a test, not a description.

1. `set` then `show` returns the tier that was set.
2. `set` twice with the same tier: both `200`, second reports `changed: false`.
3. `set` with a different tier: reports `changed: true` and the previous tier.
4. `set --tier off` creates a row that `list` shows, distinct from no row at all.
5. Every subject form in `tests/subject_corpus.h` that the schema accepts is accepted
   by `set`; every form it rejects is refused `400` **client-side**, without a round
   trip.
6. `revoke` on a live grant: `200`, `changed: true`, and a subsequent write attempt by
   that subject is denied.
7. `revoke` twice: second is `200`, `changed: false`.
8. `revoke` on a nonexistent grant: `404`.
9. `revoke` output states that one already-minted token may still be usable.
10. `set` after `revoke`: `200`, `changed: true`, `was_revoked: true`, and the subject
    can write again.
11. After (10) there is exactly ONE row for that `(server, team, subject)` — asserted
    against the table, not inferred from the CLI.
12. After (10), `show` reports `was_revoked`, and the row no longer appears as revoked
    in `list --include-revoked`.
13. The `kb_audit_event` sequence for grant → revoke → grant is three events in order,
    each with its actor: continuity lives there, not in the row.
14. `set` after `revoke` with a DIFFERENT tier applies the new tier.
15. `list` filters AND together. Default contains only live grants;
    `--include-revoked` contains live AND revoked ones.
16. `list` ordering is stable across runs.
17. Over TCP, every one of the four commands is refused `403` — including the two
    read-only ones, since a remote caller learning the grant table is itself a
    disclosure.
18. Each mutation appends exactly one audit event; each read appends none.
19. `set` for a subject who is not a member of the team succeeds, prints the
    non-member warning, and the grant is inert until membership exists — at which
    point it takes effect with no further `set`.
20. `make cli-v1-routes-check`, `v1-method-coverage-check` and
    `cli-help-coverage-check` all pass, i.e. the commands are routed, covered and
    documented.

## Open question for review

**Should `set` require the team to already contain the subject as a member?**

The intent writer reads the grant under FORCE RLS as the subject, so a grant for a
non-member is invisible and useless — creating one is harmless but silently inert,
which is a confusing thing to allow. Arguments both ways:

- **Require membership**: an operator gets an immediate, actionable error rather
  than a grant that appears in `list` and does nothing.
- **Do not require it**: grants and memberships are then orderable independently,
  which matters when provisioning a new user in one script.

Recommendation: **warn, do not refuse.** `set` succeeds and prints
`warning: alice is not currently a member of team 770001; this grant has no effect
until they are`. That keeps provisioning order free while making the inert state
impossible to miss. Flagging it because it is the one remaining judgement call in
this spec rather than a consequence of the proposal.

Round 2 of review treated this as reducible to a follow-up once the grant-lifecycle
question was settled, and did not overrule the recommendation. It stands, and is
acceptance criterion 19.
