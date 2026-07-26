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

**What a `subject` looks like.** This is the field you will get wrong if nobody
tells you, because a grant for the wrong spelling is silently a grant for nobody.
A subject is an authenticated identity in one of four forms:

| Form | When | Example |
|---|---|---|
| `<username>` | the local-PAM login — a plain host account name | `alice` |
| `oidc:<iss>:<sub>` | the OIDC login | `oidc:https%3A//idp.example:alice` |
| `cert:<issuer>:<serial>` | a machine identity from an mTLS client certificate | `cert:CN=aimee-ca:a1b` |
| `owner` | the single-org bearer principal | `owner` |

The OIDC and cert forms are namespaced by the authority that vouched, because a
`sub` is unique only within its issuer and a serial only within its CA — the `:`
inside an issuer URL is percent-encoded so the delimiters stay unambiguous. A
PAM username carries no prefix: the host is the only authority, and the two login
modes are mutually exclusive, so there is nothing for it to collide with.

**One name is reserved.** A host account named literally `owner` cannot be
granted — it is indistinguishable from the bearer principal. If you have such an
account, it needs a different name or a different login mode.

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

**Which login a user gets is not a choice you make per user — it is a property of
the kb.** The two modes are mutually exclusive:

- **An OIDC issuer is configured** → OIDC, and PAM is off. Set
  `AIMEE_KB_OIDC_LOGIN_CLIENT_ID`, `AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL`,
  `AIMEE_KB_OIDC_LOGIN_TOKEN_URL`, `AIMEE_KB_OIDC_LOGIN_REDIRECT_URI` and
  `AIMEE_KB_OIDC_ISSUER`. The issuer is shared with the bearer verifier on
  purpose: the issuer a login trusts and the issuer a token is checked against
  must not be able to drift apart.
- **No OIDC issuer** → the local PAM login, which is the one already used for
  browser sign-in.

`GET /v1/identity/auth-mode` reports which one a given kb is offering. A client
is expected to ask rather than assume, because the flows differ — one redirects
to an identity provider, the other collects a password.

The client secret is **not** an environment variable. It is vault-custodied and
read only at the moment of the code exchange, so it is never sitting where a
crash dump or a `ps` would reach it. Store it before anyone tries to log in:

| where | value |
| --- | --- |
| vault agent | `oidc` |
| vault cred | `oidc_login_client_secret` |

A kb with an OIDC profile but no stored secret answers the callback with
`503 oidc login is not fully configured` and logs `kb.oidc.login`. That is
deliberately distinguishable from a failed login — it is a deployment fault, not
an authentication one, and `auth-mode` already advertises that the kb offers OIDC.

### The login routes

| route | mode | purpose |
| --- | --- | --- |
| `GET /v1/identity/auth-mode` | both | which mode this kb offers |
| `POST /v1/identity/login/start` | OIDC | `{server_id}` → `{authorize_url, redirect_uri}` |
| `GET /v1/identity/login/callback` | OIDC | the IdP's redirect; `?code=&state=` |
| `POST /v1/identity/login/pam` | PAM | `{username, password, server_id}` |

All four are **pre-auth** by necessity — they are how a caller with no credential
gets one.

**The modes are enforced, not just reported.** A kb with a working OIDC profile
answers `POST /v1/identity/login/pam` with `409` and never consults PAM. This
matters if you are migrating: the moment an OIDC profile becomes valid, host
passwords stop working as a way in, by design. If they still worked, an IdP's MFA,
lockout and account-disable policy would be bypassable by anyone with a local
account. Conversely a kb with no OIDC profile answers the two OIDC routes with
`503`, so a client that guessed wrong gets a clear answer rather than a hang.

**Every OIDC callback failure answers identically** — `401 the login could not be
completed` — whether the state was unknown, the IdP refused the code, the
signature failed or the nonce belonged to another login. The distinctions are in
the kb log under `kb.oidc.login`, not in the response, because reporting them
would tell an unauthenticated caller which check failed. The same applies to the
password route: a wrong password, an unknown account and a username outside the
subject grammar all answer `401 authentication failed`. **If you are debugging a
login, the log is the only place the reason exists.**

**The password route is not rate limited.** kb's limiter is applied on the
bearer-gated path, and this route is pre-auth. If you expose a PAM-mode kb beyond
a trusted network, put throttling in front of `POST /v1/identity/login/pam`.

If an OIDC profile is configured but unusable — a typo in the endpoint, a
cleartext `http://` URL — the kb falls back to the PAM login and logs a warning
naming the problem. It does not report a mode nobody can complete a login with.
Check the kb log for `kb.oidc.login` if a deployment you configured for OIDC is
offering passwords.

**Tokens are short-lived and single-use.** A token is bound to one server
(`aud`), carries its own `jti`, and is consumed on first use — a captured token
cannot be replayed, and the server refuses if its replay store cannot confirm
freshness. Clients are expected to obtain tokens as needed rather than caching
one.

**Provisioning the token authority needs a raised `RLIMIT_MEMLOCK`.** Tokens are
signed under vault custody, so the authority has to be provisioned before any
user can be issued one. The two tools that do it —
`aimee-kb-token-roots-provision` and `aimee-kb-jwks-publish` — `mlockall()` at
startup so signing key material can never reach swap, and that call fails with
`ENOMEM` whenever `RLIMIT_MEMLOCK` is below the process size. A libpq + OpenSSL
binary needs more than the common 8MB default, so on a stock container both tools
exit immediately.

Raise it on whatever host runs them:

| Host | What to do |
|---|---|
| bare metal / VM | `ulimit -l unlimited` in the unit or shell that invokes them (systemd: `LimitMEMLOCK=infinity`) |
| LXC / Proxmox container | add `lxc.prlimit.memlock: unlimited` to `/etc/pve/lxc/<ctid>.conf` and restart the container — the limit cannot be raised from inside |
| Docker | `--ulimit memlock=-1:-1` |

They report `hardening (mlockall; raise RLIMIT_MEMLOCK)` when this is the cause,
so you do not have to guess which of the startup locks failed.

Three more requirements of those tools, none of them obvious from a usage line
and all of them deliberate:

- **The KMS helper and its HWM public key must be root-owned files on a path
  whose every parent directory is root-owned and not group- or other-writable.**
  That rules out `/tmp` (mode `1777`). It stops an unprivileged user substituting
  the helper under a path root is about to execute.
- **The helper's own configuration must be baked into the file**, not passed in
  the environment: both tools `clearenv()` down to the four `AIMEE_VAULT_KMS_*`
  variables before forking it. That is why the setting names a *file* rather than
  a command line.
- **They connect as a login role that is a member of `aimee_kb_migrate`**, then
  `SET ROLE` to their own provisioning role. `aimee_kb_migrate` is itself
  `NOLOGIN` — DDL authority is deliberately not something you can log in as — and
  the schema creates no login role for you, because naming it is your choice.

`scripts/run-identity-mint-e2e.sh` is a worked example of all of the above,
including a signed-HWM helper standing in for a hardware signer. Note that each
custody key needs its **own** monotonic HWM counter: the tools provision three
roots, and a shared counter makes the second root observe the first one's advance
and fail verification.

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

## 0.3.0 — PAM authentication now actually compiles in (Linux)

**What changed.** aimee auto-detects `libpam` and sets `-DWITH_PAM` when it is
present. On Linux that detection has never fired: the probe piped an
`#include` line to the compiler, and inside make's `$(shell ...)` the escaped
`\#` reached the compiler as a literal backslash, so the test failed regardless
of what the host had installed. `-DWITH_PAM` was therefore never set, and no
shipped binary linked `libpam` even though `-lpam` was on the link line.

**Does this affect me?** Only if you use the **local dashboard's HTTP Basic
Auth** on Linux. That path validates credentials through PAM, and with PAM
compiled out it took the "PAM not available — reject all credentials" branch, so
it rejected every login. It failed closed, not open: nobody got in who should not
have. But if you had concluded the dashboard's Basic Auth was broken or
unusable, this is why.

**What to do.** Nothing, unless you had worked around it. After upgrading, the
dashboard authenticates against the host's `aimee` PAM service as originally
intended, so an account that was previously refused will now succeed. If you
relied on the dashboard being effectively closed to everyone, gate it at the
network instead — that was never the intent of the setting.

Builds on hosts *without* `libpam` are unchanged: PAM stays compiled out and the
credential check still rejects everything rather than degrading to something
weaker.

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
