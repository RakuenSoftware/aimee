# Proposal: P5 — OIDC control plane: aimee-kb manages aimee-servers

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P1 (identity/teams). Shares OIDC/identity work with P1/P4.

## Thesis

Because every aimee-server already depends on aimee-kb for org egress (P2), kb is
already the org-trusted tier that sits *above* the fleet. This packet uses that
same trust relationship, pointed the other way, so **aimee-kb can manage
aimee-servers** — enroll them, list them, see their health, and (policy-permitting)
drive their admin API — with the operator authenticated by **OIDC**. Management is
a first-class capability, not gated on any single wiring detail.

## Goal

1. aimee-kb (and kb-console) can connect to the org IdP via OIDC (operator SSO).
2. aimee-servers enroll into a kb **server registry**; kb can list them and read
   their status/health.
3. From the OIDC-authenticated console, an operator can reach a server's existing
   admin API (agents, config, health) through kb, with the OIDC identity carried to
   the server — not collapsed to a shared bearer.

## §0 What already exists (and the gap)

- **OIDC verifier + console SSO** — `src/kb/auth_oidc.c`, `kb-console/auth.go`
  (RS256/JWKS, break-glass path). "Connect kb to OIDC" is largely built for humans
  already.
- **PKI + enrollment** — `src/kb/enroll.c`, `src/kb/pki.c`: single-use `aimee://`
  tokens, CSR→CA-signed `cert:CN`. Today enrollment flows **client→KB**; P5 reuses
  the exact machinery to enroll **servers into a registry**.
- **Server admin API already exists, per-server** — `src/server/server_http_routes.c:1770+`:
  `/v1/agents`, `/v1/agent/{add,remove,enable,disable,set,…}`, `/v1/config/{get,set}`,
  health, delegate, cron — behind bearer/scope/`remote_writes` gating.
- **The three real gaps** (from the control-plane survey):
  1. **No server registry / no KB→server management channel.** KB is a callee; its
     only outbound to servers is a cache-invalidation push
     (`src/kb/kb_curator_notify.c`).
  2. **OIDC identity terminates at the console** — `kb-console/proxy.go` forwards
     only the shared `console-admin` bearer ("the browser's own token is never
     forwarded"); it never reaches a server.
  3. **kb-console's allowlist is KB-only, deny-by-default** (`kb-console/acl.go` +
     `src/kb/http/kb_route_acl.c`) — it cannot address a server route.

## §1 Server registry (kb-side)

A `server` registry table in DB2: `{server_id, cert_cn, owner (OIDC sub, nullable),
team_id, last_seen, health, version, endpoint}`. **Every aimee-server has its own
individual mTLS cert** — one cert per server, issued from kb's CA via the existing
`aimee://` flow (`kb_enroll_mint`/`kb_enroll_redeem_csr`), each with a unique
`cert:CN`, and each independently revocable (`cert.revoke`). The redeemed `cert:CN`
*is* the server's identity and the registry key; redemption inserts the registry
row. No shared/fleet cert — revoking or rotating one server never touches another.
Servers heartbeat (reuse the existing server→kb `/v1/health` client in
`kb_client.c`, over mTLS) to keep `last_seen`/`health`/`version` fresh. The `owner`
(OIDC sub) is recorded when OIDC is configured and left null otherwise — the
registry does not depend on OIDC.

## §2 kb→server management channel

An outbound client from kb to a registered server (mirror of the existing
`src/server/kb_client_mtls.c` transport, reversed). **This channel is always mTLS**,
both directions: kb presents its own cert, and it connects to the server's pinned
per-server cert from the registry (§1) — never bearer-only. The server authenticates
kb as a `cert:CN` principal with a management scope and honors it under its existing
`remote_writes`/capability gating (`server_http.c:334-395`) — so the server stays in
control of what it permits. No new auth model on the server; just a new trusted
mTLS caller. OIDC, when present, only names the *operator* behind a kb-originated
action (§3); the channel itself never depends on it.

## §3 OIDC identity propagation

Stop collapsing identity at the console. When an operator drives a server action,
kb mints a short-lived token (or a scoped `cert:CN`) carrying the operator's
identity, so the server records *who* acted, not a shared `console-admin`. That
identity is the operator's OIDC `sub` when OIDC is configured, and the
owner/bearer/console-admin identity otherwise — the propagation mechanism is the
same either way. This closes gap (2) and gives the server-side audit log a real
actor without requiring OIDC.

## §4 Console surface

Extend the console allowlist (`acl.go` + `kb_route_acl.c`) with a **server-scoped**
family: list servers (registry), view a server's health/agents/config, and — behind
an explicit org-admin capability and the target server's own `remote_writes` policy
— drive its admin endpoints via §2. New console views: **Fleet** (server list +
status) and a per-server drill-down. OpenAPI + `v1-method-coverage` for the new kb
routes.

## Acceptance criteria

- A server enrolls and appears in the registry with its `cert:CN`, owner, team,
  version, and a fresh heartbeat.
- An OIDC-authenticated operator lists the fleet and reads a server's agents/health
  through kb.
- A management action reaches the server as the operator's propagated identity and
  is recorded as that actor in the server audit log — not as `console-admin`.
- A server with `remote_writes: off` refuses a management write even from kb
  (server stays authoritative over its own policy).
- Break-glass console login still works if OIDC is down (existing path preserved).

## Testing

Unit: registry CRUD, enrollment→registry insertion, identity-propagation token
minting/verification, allowlist extension (server routes permitted, others still
denied). Integration: enroll a real server into a kb, list it, drive an agent
enable/disable through kb, assert the actor in the server's audit log, and assert
`remote_writes: off` blocks the write.

## Non-goals

No config *push* fan-out / templating across many servers in this packet (registry
+ single-server drive first; bulk orchestration is a follow-up). No replacing the
server's local auth — kb becomes a trusted caller, not the server's owner. No SAML.
