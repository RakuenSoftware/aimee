# Upgrading aimee

Notable user-facing changes, newest first. Each entry says what changed, whether
it can break an existing deployment, and what to do about it.

---

## 0.3.0 — `/v1` writes are authorized per user, not per deployment

**This is why the release is 0.3.0 and not 0.2.x.** Two things that authorize
writes today stop authorizing them. An appliance that upgrades without acting
will accept reads and refuse every remote write. That is deliberate — the
alternative is silently carrying a deployment-wide write switch into a release
that claims per-user authorization — but it is not a change you want to discover
from a monitoring alert.

**What changed.** `aimee.api.remote_writes` was a single process-global switch:
set it to `data` or `full` and *every* caller holding the shared bearer got that
tier. There was no way to say "Alice may write, Bob may not". Write authority is
now a property of the authenticated **user**, carried in a short-lived,
kb-signed identity token and checked on every `/v1` request.

**Does this affect me?** Yes, if any of these are true:

- you set `aimee.api.remote_writes` to `data` or `full`;
- anything writes to `/v1` over TCP — thin clients, webchat, scripts, cron jobs,
  CI, audit tooling;
- you rely on the standing shared bearer (`AIMEE_API_BEARER`) for writes.

If your deployment is read-only over TCP, or drives aimee exclusively over the
local UDS socket, this change is transparent.

**What stops working.**

| Was | Now |
|---|---|
| `aimee.api.remote_writes=data\|full` authorizes writes for everyone | Parsed, but authorizes nothing. Startup warns; `remote_writes.global_ignored` counts requests that would formerly have been allowed by it |
| Standing shared bearer / `AIMEE_API_BEARER` authorizes writes | Authorizes **reads only**. The reads it permits today are unaffected |
| No per-user distinction | Each `(server, team, subject)` carries its own tier: `off`, `data`, or `full` |

The *one-time bootstrap* bearer is untouched. It has always been rotate-only —
`handle_api_rotate_bearer` refuses every other TCP route until it is rotated — so
no read path ever depended on it.

**The local UDS operator is unaffected and cannot be locked out.** A connection
on the unix socket is the same-user trusted peer: it returns full capability and
never consults the per-user tier. That is the recovery path if you get the
configuration wrong, and it is why the appliance can always be repaired from a
shell on the host.

**What to do, in order.**

1. **Before upgrading**, list who actually writes over `/v1`. Every one of them
   needs a grant or a replacement credential.
2. **Upgrade.** No grants exist yet, so remote writes are refused. Reads continue.
3. **Grant the users who need to write.** Grants live in `kb_write_tier_grant`,
   keyed by `(server_id, team_id, subject)`, and are written through the
   `kb_write_tier_grant_set` / `kb_write_tier_grant_revoke` functions — direct
   `INSERT`/`UPDATE` is deliberately not available to the runtime role, because
   those functions also write the tamper-evident audit record for the change.
4. **Verify** that the users you expect appear, with the tiers you expect, before
   you tell anyone the upgrade is done.

**The first grant.** Grants are administered by an org admin or the team's lead —
but on a freshly upgraded appliance there may be neither, so the local operator
is the root of trust. It installs the principal `owner`, which counts as admin.

One detail that will otherwise cost you an afternoon: the operator installs its
context with **team `0`**, not the team it is about to grant into. Team
membership is only enforced for a team greater than zero, and `owner` is a member
of no team, so passing the real team id fails with *"team not in principal
memberships"*. Passing `0` is what makes the operator un-lockout-able. This is
covered by an automated test precisely so it does not regress.

**Replacing non-interactive callers.** Anything that writes without a human —
cron, CI, audit tooling, service integrations — needs a service-account token
rather than the shared bearer: the same token type and claim set, with a service
`sub`, a fixed tier, and a longer expiry. Give each integration its own subject
rather than sharing one, or you lose exactly the per-user attribution this
release exists to provide.

**Interactive callers** (thin clients, webchat) obtain a token by logging in
through the adoption wizard.

**Tokens are short-lived and single-use.** A token is bound to one server
(`aud`), carries its own `jti`, and is consumed on first use — a captured token
cannot be replayed, and the server refuses if its replay store cannot confirm
freshness. Clients are expected to obtain tokens as needed rather than caching
one.

**Set `AIMEE_SERVER_TEAM_ID`.** This release adds one required variable: the id
of the team this server serves, the same registry row `AIMEE_SERVER_ID` comes
from. Set them together.

If it is unset the server still **starts and serves reads**, and denies every
write — deliberately, because refusing to boot would take reads down over a
write-authorization setting and would disable the local-operator recovery path
you may need. It logs an error naming the variable at startup, and every denial
reports `no_team_configured` rather than blaming the caller's token.

**If writes are still refused after granting**, the server distinguishes the
reasons rather than returning a single opaque denial. Each is logged with the
request id. Check for:

| Reason | Meaning |
|---|---|
| `absent` | no identity token presented (an ordinary read-only caller) |
| `invalid` | malformed, bad signature, wrong `iss`/`aud`, or outside its validity window |
| `unknown_kid` | signed by a key this server has not fetched yet |
| `wrong_team` | a valid token for a team this server does not serve |
| `no_team_configured` | **this server** is missing `AIMEE_SERVER_TEAM_ID` — not a token problem |
| `replay` | this token's `jti` was already used |
| `replay_unavailable` | the replay store could not confirm freshness, so the write was refused rather than assumed safe |

`aimee api status` reports `remote_writes.global_ignored` once it is non-zero:
the number of requests refused that the retired global would formerly have
allowed. It counts only those, not denials in general, so it measures what this
cutover is actually costing you.

---

## 2026-07 — The `plugin-loader` is removed

**What changed.** aimee's built-in `plugin-loader` subsystem has been removed
entirely. There is no longer a plugin discovery/registry/enable-disable
mechanism inside aimee, and the endpoints, config keys, and CLI that drove it are
gone. Extensibility in aimee is delivered through **hooks**, **MCP tools**, and
**skills** (all documented in `MANUAL.md`) and, for maintainers, through
first-class **modules** — not through a separate plugin loader.

**Does this affect me?** Only if you actively used the plugin loader. Concretely,
you are affected if any of these applied to your deployment:

- you set `AIMEE_ENABLE_PROJECT_PLUGINS`;
- you shipped a project-local plugin manifest or plugin directory for aimee to
  discover;
- you called the plugin HTTP routes — `GET /v1/plugins`, `POST /v1/plugins/enable`,
  `POST /v1/plugins/disable`, or `GET /v1/dashboard/plugins`;
- you scripted the plugin management subcommands.

If none of those applied, this change is transparent — a normal upgrade needs no
action.

**What was removed.**

| Removed | Replacement |
|---|---|
| `AIMEE_ENABLE_PROJECT_PLUGINS` env var | — (no equivalent; use a mechanism below) |
| `GET /v1/plugins`, `POST /v1/plugins/{enable,disable}` | — (now `404`) |
| `GET /v1/dashboard/plugins` | — (now `404`) |
| Plugin discovery of project-local plugin manifests | MCP tools / hooks / skills |
| Plugin management CLI | — |

The `/v1/openapi.yaml` served by `aimee-server` no longer lists any plugin route,
so generated clients pick the change up automatically.

**What to do instead.** Pick the mechanism that matches what your plugin did:

- **You added a tool the model could call.** Expose it over **MCP** and register
  the MCP server with aimee. This is the supported way to add callable tools and
  works with every client. See *§16 Skills and toolsets* and the MCP integration
  notes in `MANUAL.md`.
- **You intercepted or post-processed tool calls / injected context.** Use the
  client **hooks** aimee already registers — `SessionStart`, `PreToolUse`,
  `PostToolUse` — described under *Hooks* in `MANUAL.md`. These cover the
  interception and context-injection cases the plugin `pre-LLM` hook was used for.
- **You bundled reusable prompts/procedures.** Package them as a **skill**
  (`AIMEE_BUNDLED_SKILLS_DIR` still overrides the bundled-skills location).
- **You are a maintainer extending aimee's own binary.** Add a first-class
  **module** under `src/modules/<id>/` with a `module.yaml` descriptor, rather
  than a loadable plugin. See `docs/modules/` and `docs/refactor-baselines.md`.

**Note on Codex.** The "local plugin" line for the Codex CLI in `MANUAL.md` /
`docs/COMPATIBILITY.md` refers to *Codex's own* plugin mechanism, not aimee's
plugin-loader, and is unaffected.

---
