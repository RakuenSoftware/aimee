# Operator CLI for per-user `/v1` write grants — command spec

Increment 5 of [per-user-remote-writes-authz](per-user-remote-writes-authz.md).
Written for review **before** implementation, because the previous roundtable
blocked this increment on the absence of exactly this document.

## Why this exists

§7 of the proposal makes the local UDS operator the irreducible root of trust and
says that operator "configures kb (OIDC issuer profile *or* PAM + the
`{subject → team, tier}` grants)". §6 adds that after upgrading, "operators
populate grants post-upgrade (documented procedure); we do **not** auto-map the old
global into a wildcard grant".

Both sentences describe work with no tool to do it. Today a grant can only be
created with hand-written SQL against `kb_write_tier_grant` as a role with INSERT
on it. That is the gap: **the upgrade is fail-closed, so every deployment that
takes this change has zero grants and every remote write is denied until somebody
writes SQL.**

## Scope

In scope: create, revoke, list and inspect per-user write-tier grants.

**Explicitly NOT in scope**, so the boundary is a decision rather than an
oversight:

| excluded | why |
| --- | --- |
| Creating teams or memberships | `kb_team` / `kb_team_membership` are existing surfaces with their own lifecycle. A grant references them; managing them is not this command's job. |
| Registering servers | `kb_server_registry` rows come from enrollment, which is a certificate flow. A grant against an unregistered server is refused by the intent writer, which is the correct place. |
| Minting tokens | That is the login routes plus the token authority. An operator granting a tier must not be able to obtain somebody else's token as a side effect. |
| Editing a grant in place | There is no `grant edit`. Changing a tier is `set` (idempotent upsert); removing authority is `revoke`. A partial edit would need its own audit semantics for no benefit. |
| Bulk import from the old global flag | Deliberate, per §6. Auto-mapping `aimee.api.remote_writes: full` to a wildcard grant would re-introduce a global authorizer. The operator decides who gets what. |
| Un-revoking | `revoke` sets `revoked_at`. Restoring access is `set` again, which is a new grant with a new audit trail. Resurrecting a revoked row would make the audit log lie about continuity. |

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

**Revocation is not retroactive to tokens already minted.** A token lives at most
`300s` and is single-use, so the exposure is bounded, but `revoke` must say so in
its output rather than let an operator assume it cut off an in-flight session:

```
revoked alice on mintsrv (team 770001)
  note: tokens already minted remain valid until they expire (<= 300s) or are used
```

### `aimee kb grant list`

```
aimee kb grant list [--server <id>] [--team <id>] [--subject <s>]
                    [--include-revoked] [--json]
```

Every filter is optional and they AND together. Default hides revoked rows;
`--include-revoked` shows them with their `revoked_at`. Output is sorted by
`(server_id, team_id, subject)` so a diff between two runs is meaningful.

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

**The local UDS operator only.** These commands mutate who may write to a remote
server; that authority belongs to the root of trust §7 already defines as
un-lockout-able, and to nothing reachable over TCP. Concretely: the route requires
`is_tcp == 0`, the same condition `server_http_conn_caps` uses to return
`CAPS_ALL`.

This is a deliberate refusal to add a remote grant-management API. A remote
operator with a `full` tier could otherwise grant themselves — or anyone —
`full` on any server in their team, which makes the tier system decorative. If
remote administration is ever wanted it needs its own design with its own
delegation model, not a flag on this command.

## Audit

Every mutation writes a `kb_audit_event` through the existing
`kb_audit_worm_append`, with the actor being the resolved UDS operator identity.
`list` and `show` do not — they are reads, and auditing them would bury the
mutations.

## Acceptance criteria

Each of these is a test, not a description:

1. `set` then `show` returns the tier that was set.
2. `set` twice with the same tier: both `200`, second reports `changed: false`.
3. `set` with a different tier: reports `changed: true` and the previous tier.
4. `set --tier off` creates a row that `list` shows, distinct from no row at all.
5. Every subject form in `tests/subject_corpus.h` that the schema accepts is
   accepted by `set`; every form it rejects is refused `400` **client-side**,
   without a round trip.
6. `revoke` on a live grant: `200`, `changed: true`, and a subsequent write
   attempt by that subject is denied.
7. `revoke` twice: second is `200 changed: false`.
8. `revoke` on a nonexistent grant: `404`.
9. `revoke` output states that already-minted tokens survive.
10. `list` filters AND together; default hides revoked; `--include-revoked` shows
    them.
11. `list` ordering is stable across runs.
12. Over TCP, every one of the four commands is refused `403` — including the two
    read-only ones, since a remote caller learning the full grant table is itself
    a disclosure.
13. Each mutation appends exactly one audit event; each read appends none.
14. `make cli-v1-routes-check`, `v1-method-coverage-check` and
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
impossible to miss. Flagging it here because it is the one decision in this spec
that is a judgement call rather than a consequence of the proposal.
