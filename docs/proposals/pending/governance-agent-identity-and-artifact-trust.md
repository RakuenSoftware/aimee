# Proposal: Per-agent identity, delegation chains, fleet registry, and signed executable artifacts

- **State:** PENDING — Part 3 of the three-part governance arc
  ([attestable enforcement](governance-attestable-enforcement.md) +
  [governance posture & policy surface](governance-policy-surface-and-posture.md)
  + this).
- **Origin:** autonomous overnight governance deep-dive commissioned by JBailes,
  2026-07-13.
- **Charter roles:** Enforce / Classify-Score / Gate-Promote.
- **Builds on:** shipped mTLS client identity
  ([mtls-client-identity](../done/mtls-client-identity.md)), the credential vault
  ([cred-vault-consolidation](../done/cred-vault-consolidation.md)), and the WORM
  structured actor model
  ([auditable-worm-audit-store](../done/auditable-worm-audit-store.md) §1).

## Thesis

Parts 1 and 2 make enforcement attestable and posture coherent — but a chained
verdict row is only as meaningful as its `actor`, and a policy is only as
meaningful as the code it lets run. Both ends are currently soft:

**Identity.** aimee has real transport attestation — UDS peercred → `uid:<n>`,
mTLS → `cert:<CN>` (server-applied prefix, sanitized charset), webchat →
`webuser:<name>` — but the *agent* layer above it collapses:

- delegates and autonomous runs fall back to the **shared server principal**
  (`docs/SECURITY.md` principal model; delegate launches resolve vault creds as
  the server, not as themselves);
- a networked bearer client is **anonymous** (all-or-nothing server principal,
  pre-mTLS TOFU);
- the governed-action audit actor is just the coarse role `primary|delegate`
  (`src/guardrails_action_audit.c:119`);
- cross-client memory scoping trusts an **unauthenticated env var**
  (`AIMEE_HOOK_CLIENT`), a load-bearing boundary the memory-interception
  close-out explicitly flagged for multi-principal use.

So a chain row can prove *a delegate* did something, but not *which* delegate,
on *whose behalf*, with *which* granted capabilities — the exact identity-abuse
surface OWASP files as ASI03, and the layer the whole industry spent 2025–26
building (per-agent principals as first-class IAM citizens, delegation expressed
as nested actor claims, org-wide agent registries with ownership and lifecycle).
aimee needs no new cryptography for this — the substrate exists; the identities
just aren't minted, propagated, or required.

**Artifact trust.** Plugins load from a `.aimee-plugin` manifest with **no
signature, checksum, or pinning**; skills (`skills/*/SKILL.md`), ensemble
templates (`ensemble_templates/*.json`), and saved workflow definitions execute
equally unsigned. These artifacts steer agents that hold credentials and run
default-on autonomous pipelines: they are the supply chain (ASI04), and today it
is entirely trust-on-nothing. (Contrast: the vault refuses plaintext key writes
over unattested transport — the *credential* chain is hardened while the *code*
chain is open.)

## Deltas

### C1 — Per-agent principals

Mint a distinct principal at every delegate/autonomous launch:
`delegate:<name>:<job-id>` (charset per the existing CN sanitizer), carried in
the launch context and used — instead of the server principal — for:

- vault credential resolution (the existing `resolve_token` precedence gains a
  per-principal step; grants stay scoped, so "which delegates may touch the
  forge token" becomes a vault capability question);
- the WORM actor `principal_id` and lifecycle events;
- scope-bound bearers for anything the delegate calls back (the
  `scope:<kind>:<id>:<secret>` kind already exists — reuse it as the delegate's
  session credential instead of an ambient server bearer).

Autonomous runs get `system:trigger:<rule>` principals so "the watch-dir
pipeline did X" is distinguishable from "the operator's session did X".

### C2 — Delegation chains in the audit actor

A verdict row answers "who ultimately authorized this" by carrying the chain,
not just the leaf: extend the audit actor with `on_behalf_of` — an ordered list
assembled **server-side at dispatch** (never self-reported by the agent), e.g.
`operator uid:1000 → primary session S → delegate:reviewer:J42`. This mirrors
the nested-actor-claim pattern from token-exchange without new token formats:
the server already knows every hop because it launched every hop. Sub-delegate
dispatch appends; depth is bounded by the existing delegation limits. Rows stay
within the 16 KB detail budget (principals are short ids).

### C3 — Fleet registry: no shadow agents

One queryable inventory of everything that can act, so the *absence* of
governance is visible:

- **armed workflows + trigger rules** (source, mode, workspace, last fire);
- **registered delegates** (toolset, backend local/docker, credentials granted);
- **hooked clients** — session-start already POSTs to the server; register
  `{client, host, session, hook version}` so "which harnesses are actually
  mediated" is a query, and an agent acting through the gateway *without*
  registered hooks is flaggable (the observability answer to the un-mediated
  channel, which runtime policy explicitly does not chase);
- **configured MCP servers / ingress clients** per config.

Surface it through the pending
[operator-audit-activity-surface](operator-audit-activity-surface.md) rendering
work. Registry mutations (new trigger rule, new delegate, new plugin) write
chain rows, and — for autonomous sources — require a `decision_log` entry naming
the owner, tying fleet membership to the shipped decision-record machinery. The
registry answers OWASP ASI10's first question: *how would you know a rogue agent
exists?*

### C4 — Signed executable artifacts

Content-hash manifests + signatures for the four artifact classes (plugins,
skills, ensemble templates, saved workflow definitions):

- **Manifest**: per-artifact `sha256` over a canonical file list; the loader
  computes and compares before use. Cheap, no key infrastructure, catches drift.
- **Pinning (`standard` profile)**: first load TOFU-pins the hash (the pattern
  aimee already uses for cert pinning + bearer enrollment); a changed hash
  refuses to load until an operator re-approves (webchat gate), which re-pins
  and writes an `artifact.approve` chain row.
- **Signatures (`hardened` profile)**: artifacts must carry an ed25519 signature
  from a key in the operator's trust set (reuse the anchor-key machinery from
  Part 1 A4); unsigned artifacts refuse to load. Signing is one CLI verb
  (`aimee artifact sign <path>`), so first-party artifacts cost nothing extra.
- Load-time verdicts (`artifact.load` allow/deny) are chain rows either way.

### C5 — Authenticate the hook channel

Replace trust-the-env with a minted secret: session-start returns a
session-scoped token; subsequent hook invocations present it; the server binds
`{client, session, principal}` to that token. Closes the flagged
`AIMEE_HOOK_CLIENT` spoof (one session claiming another client's scope) with
plumbing that already exists at both ends (session-start POST, hook → server
report path). Unauthenticated hook calls degrade to an untrusted default scope
under `standard`, refused under `hardened`.

## Sequencing

C1 → C2 (chains need per-agent principals to be worth recording); C3 and C4 are
independent of both and of each other; C5 is independent and small. Suggested
order: C5, C1, C2, C3, C4 — cheapest trust fix first, registry before signing so
the inventory exists before it is enforced against.

## Non-goals

- **OS-level sandboxing of delegates** (seccomp/MAC/VM). Worktree + optional
  docker backend remain the isolation story; same-UID compromise stays a
  documented non-goal per `docs/SECURITY.md`. A hardened-profile "docker backend
  required for autonomous delegates" default is a one-line follow-up to Part 2's
  table, not new machinery here.
- **External identity federation** (SPIFFE/OIDC issuance for aimee agents).
  The principal model stays aimee-internal; federation is a future bridge, and
  the C1 naming scheme is chosen so it maps cleanly onto a SPIFFE path later.
- **Signing third-party marketplace distribution.** C4 protects *this
  deployment's* artifact integrity; a distribution/marketplace trust model is
  out of scope.
- **Behavioral trust scoring of agents.** Registry + identity first; reputation
  is speculative until the substrate exists.

## Risks / honest limits

- **Principal explosion**: per-job delegate principals are high-cardinality —
  the vault grants attach to the `delegate:<name>` prefix (stable), while the
  `:<job-id>` suffix exists only in audit rows; no per-job vault entries.
- **TOFU is TOFU**: C4's `standard` tier trusts first load. That is the same
  explicit tradeoff as the existing cert-pinning TOFU, and `hardened` closes it.
- **Registry freshness**: hooked-client rows are heartbeat-based
  (session-start/session-end); a crashed harness lingers until expiry — the
  registry is an inventory, not liveness truth.
- **C5 rollout**: old clients without the token must keep working during the
  window (`observe`/`standard` degrade path above) or every fleet upgrade
  becomes a flag day.

## Tests

- C1: delegate launch mints the principal; vault resolution honors per-principal
  grants (denied grant → no credential, chain row says which principal); WORM
  rows carry it.
- C2: operator→primary→delegate→sub-delegate produces a 4-hop `on_behalf_of`;
  chain is server-assembled (an agent-supplied claim is ignored).
- C3: new trigger rule without a decision record is rejected; registry lists all
  four actor classes; unregistered-hook gateway traffic is flagged.
- C4: modified plugin refuses to load under `standard` until re-approved
  (re-pin + chain row); unsigned artifact refused under `hardened`; signed one
  loads with an `artifact.load` allow row.
- C5: spoofed `AIMEE_HOOK_CLIENT` with a mismatched token lands in untrusted
  scope (`standard`) / refused (`hardened`); token-less legacy client works in
  `observe`.
