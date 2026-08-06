# governance module

## Purpose and non-goals

`governance` is the optional organizational governance plane for Aimee Control Plane. It owns federated
OIDC/SSO, organizational identities and roles, governance policy authoring/distribution, approvals and
decision records, posture/evidence views, agent/delegation identity chains, fleet governance, and
artifact-signing trust policy. It does not make core execution enforcement, audit integrity, vault
custody, transport authentication, Git, SSH, or SSHSIG optional.

## Public contracts

Governance consumes verified core principals and produces tenant-bound policy/decision artifacts for
core enforcement. OIDC uses named provider-neutral issuer profiles, not a provider enum: discovery or
explicit endpoints, client identity, vault-backed secret reference, redirects, scopes, PKCE/nonce,
accepted algorithms, and namespaced claim mappings. GitHub may be configured if it conforms, but
`GitHub` is never the governance contract or default provider.

### Go process stage

The supervised governance process uses the shared pure-Go module runtime for the
bounded response tool-policy decision. Its GOVQ/GOVR wire contract accepts at
most 16 fixed-width tool names, the already-resolved policy-active flag, and the
provider stop reason. It returns the survivor mask, intervention count, and the
same stop-reason finalization used by the legacy response policing path. Exact
`Agent`, `spawn_agent`, `RemoteTrigger`, and `Task` names are denied; matching is
case-sensitive and intentionally mirrors the existing canonical-name boundary.
The C adapter is a wire-parity fixture. Parsed-response compaction and memory
ownership, policy/config resolution, request-side policing, OIDC, identity, and
control-plane orchestration remain in their current C owners.

The descriptor's ownership fields describe what lives under `src/modules/governance/` today, which is
the response-governance stage alone: `gw_stage_governance.c`, its private header
`gw_stage_governance.h`, and the process wire-parity adapter, tested by
`src/tests/test_response_governance_stage.c` plus cross-language conformance. That is narrower than
the governance plane this document describes. The OIDC, identity, policy-distribution, and console
surfaces remain distributed across the KB, DB2, management, and console layers and are not yet
module-local. The descriptor declares its sources, private header, test, and this document and sets
`ownership_complete: true`. The latch asserts that those declarations exhaustively cover the module
root as it stands (one source and one private header), not that the broader governance plane has been
migrated. `docs/validation/core-modularization-slice-40.md` records the declaration audit and
`docs/validation/core-modularization-slice-41.md` the completeness audit; the two were split so the
latch reviews declarations that were merged on their own first. Because the module owns exactly what it
declares today, adding a new module-local source or private header without declaring it now fails CI on
`rule=ownership-complete`.

## Dependencies and consumers

- `audit`: records organizational decisions and supplies the canonical ledger for read-only projections.
- `config`: supplies activation and provider-neutral governance declarations.
- `delegates`: supplies verified delegate identity/invocation evidence without yielding execution authority.
- `execution-policy`: reauthorizes distributed policy at the action boundary.
- `gateway`: admits authorized governance/control requests.
- `ir`: carries canonical policy, identity, decision, and attestation records.
- `module-runtime`: supplies optional lifecycle, capability, and readiness state.
- `protocols`: carries typed governance APIs and adapters.
- `routing`: selects eligible governed providers without owning governance policy.
- `tools`: exposes authorized governance operations through core dispatch.
- `vault`: stores OIDC client secrets, signing material, and protected references.

Consumers include Aimee Control Plane's headless and optional web management surfaces, fleet/runtime
management, policy distribution, organizational audits, and core enforcement at verified integration
points.

## Providers and readiness

Current code is distributed across `src/kb/auth_oidc.c`, KB identity/JWKS/enrollment/account routes and
DB2 tables, management-token code, console Accounts/Governance pages, and
`src/modules/governance/gw_stage_governance.c`. Readiness must separate module selection, issuer-profile
validity/discovery, vault secret availability, JWKS freshness/rotation, claim mapping, tenant binding,
policy distribution, audit appendability, and downstream enforcement. A stored issuer URL is not ready
OIDC.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the descriptor is `enabled_by_default: false`, so governance is selected before startup and omitted/disabled surfaces are not advertised.

The `modules.governance` response-stage gate follows the descriptor's default-off selection. The legacy
`AIMEE_STAGE_GOVERNANCE` fallback is also opt-in and controls only response tool-policing, while OIDC has
separate environment/file/DB2 routes. The target module selection must gate all organizational governance
surfaces and leave core fail-closed enforcement intact. Issuer profiles are configurable from Control Plane
UI and equivalent CLI, environment/config-file, and non-web API surfaces; secrets are vault references.
With governance absent, OIDC and organizational settings are absent from advertised catalogs.

## Surfaces

Target surfaces include provider-neutral issuer-profile administration, organizational accounts/roles,
policy and approval authoring, posture/attestation evidence, fleet governance, and signed-artifact trust
through Control APIs/CLI and optional `control-web`. Current `/v1/config/oidc` accepts issuer, audience,
`jwks_url`, `admin_claim`, and `admin_values`; it is a partial provider-neutral seed, not the full target
profile contract. SSHSIG and Git surfaces remain core and merely provide verified evidence to governance.

## Data and migrations

Current stores include `kb_console_oidc`, `kb_oidc_jwks`, admin grants, memberships, enrollment and
management-token records. Target data adds named issuer profiles, namespaced claim mappings,
tenant/principal-bound decisions, distribution state, attestations, and signing-key metadata while secrets
remain in vault. Migrations are append-only, preserve `(iss,sub)` identity and tenant bindings, and
reauthorize legacy KB policy before distribution.

## Security and privacy

`OIDC` validation is fail-closed for issuer, audience, signature algorithm, nonce/PKCE, token age, JWKS
rotation/revocation, and claim mapping. Management actions require tenant-qualified identity, capability,
target/channel binding, replay protection, and audit intent/outcome. OIDC, mTLS, SSH, and SSHSIG are
distinct evidence mechanisms. Governance may evaluate verified SSHSIG/artifact evidence but cannot replace
core signature verification or create a repository-specific trust adapter.

## Supported journeys

An operator selects `governance`, configures any conforming OIDC issuer through the Control Plane or
headless equivalent, stores client secret material in vault, maps claims to tenant roles, authenticates,
and distributes an authorized policy/management decision whose core enforcement and audit remain intact.
Without governance, Control continues core shared-memory/management operation with its local reference
authenticator and exposes no organizational-governance or OIDC surface.

## Tests and failure behavior

Current suites such as `test_kb_auth_oidc` cover OIDC JWT/JWKS verification, issuer-scoped identity, console OIDC config, accounts,
ACLs, management tokens/endpoints, grants, tenancy, and response-stage policing. Missing discovery/JWKS,
invalid claims, stale or revoked keys, unavailable vault secrets, ambiguous tenant mapping, replay,
authorization failure, or audit failure must deny the governed action. Future full-minus-one tests must
prove governance objects, routes, settings, data initialization, jobs, and web pages are absent.

## Operational diagnostics

Report `governance` selection/readiness, issuer profile identifier (not tokens), discovery/JWKS freshness,
key generation, claim-mapping outcome class, tenant/principal handle, policy version, distribution state,
decision correlation, and audit result. Do not log tokens, claims beyond approved identifiers, client
secrets, signing keys, raw policies, or private attestation payloads.

## Compatibility

`AIMEE_KB_OIDC_*`, `KB_CONSOLE_OIDC_FILE`, `/v1/config/oidc`, single-row
`kb_console_oidc`, and legacy KB governance/product names require bounded migration records. Accepted
legacy input may be migration-only and unadvertised. GitHub/GitLab OAuth settings used by core Git are not
aliases for governance OIDC and must not be migrated into issuer profiles without explicit operator intent.

## Extension and removal

New identity providers implement the same standards-based issuer-profile contract; provider-specific
branches, enums, or repository adapters are rejected. Distributed OIDC, identity, policy, management,
console, and response-stage code are `relocate` or `split-owner` candidates. The response-policing stage
must be reconciled with core `execution-policy`: any non-negotiable action enforcement remains core, while
organizational policy authoring/decision semantics may move here. Removal requires caller, surface,
configuration, store, migration, and supported-journey evidence.
