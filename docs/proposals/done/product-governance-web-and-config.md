# Proposal: split Runtime and Control Plane governance, web modules, and config surfaces

- **State:** DONE — archived 2026-08-04 as partially implemented. Runtime/control web ownership and
  initial decision surfaces landed; product naming, separately hosted processes, optional lifecycle,
  and truthful effective-config parity continue in
  [`runtime-control-product-boundary-residual.md`](../pending/runtime-control-product-boundary-residual.md).
- **Historical state:** roundtable-approved 2026-07-20; awaiting project acceptance
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries-residual.md)
- **Owns:** product roles/names, Runtime and Control web lifecycles, dashboards, and advertised
  configuration behavior
- **Implementation dependencies:** module descriptors, core config/module-runtime contracts, and
  suite compatibility records
- **Date:** 2026-07-20 (reconciliation note added 2026-07-23)

> **2026-07-23 amendment reconciliation.** The product boundary (Runtime/Control), web-module
> optionality, and truthful-config ownership this proposal defines are unchanged, but the suite
> amendment
> ([`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries-residual.md))
> makes them concrete in three ways to re-check on re-review: (1) `runtime-web` and `control-web` are
> **separate programs on the shared-memory event bus**, admitted and routed like any module, not
> in-process handlers; (2) each product — Runtime and Control — **hosts its own bus** (the single
> in-source C bus host), and cross-product paths use the existing network transport, so this
> proposal's admission/topology wording should reference the bus host explicitly; (3) the advertised
> **effective configuration** surface and the runtime **capability advertisement** to clients are the
> same capability data observed at two ends — modules publish capabilities to core over the bus, and
> [`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md) projects them,
> so this proposal's config-surface ownership and that advertisement must not diverge.

## Decision

Rename the products around their actual governance boundaries and make their web interfaces
optional process modules with truthful, generated configuration surfaces.

## Product roles

**Aimee Runtime** (`aimee-runtime`) replaces `aimee-server`. It is the interaction boundary for one
user: gateway sessions, agent execution, local tools/workspace, routing, response delivery, and
personal memory integration. It remains one-user-per-instance.

Runtime admission carries one opaque `runtime_principal` handle established at composition; the
gateway contract accepts no principal collection, tenant selector, or per-request principal
replacement. Control uses the distinct multi-tenant admission contract.

**Aimee Control Plane** (`aimee-control`) replaces `aimee-kb`. It is no longer described as merely
a knowledge base. It is the multi-tenant management and governance boundary for shared memory,
identity, policy distribution, fleet/runtime management, provider governance, audit visibility,
and optional KB synthesis. Knowledge-base functionality remains a domain inside Control.

Control is the composition host, not the owner of every capability it presents. Optional
`governance` owns OIDC/SSO, organizational identities and roles, governance policy authoring and
distribution, approvals/decision records, posture profiles, attestation exports, agent/delegation
identity chains, fleet governance, and artifact signing/trust. `control-web` renders those surfaces
only when governance is selected and active; headless governance remains available through CLI,
environment/configuration, and non-web APIs. With governance omitted, Control still supports its
core multi-tenant shared-memory and management contracts using core principal/tenant handles and a
tenant-scoped local reference authenticator, but exposes no OIDC or organizational-governance
surface through CLI, environment/configuration, API, or web. Its audit projection remains a
deterministic read-only view sourced directly from the core audit ledger.

OIDC is a provider-neutral governance contract. Control manages named issuer profiles rather than
shipping provider-specific modes. A profile declares standards-based issuer discovery or explicit
authorization, token, user-info, JWKS, and end-session endpoints; client identity; a vault-backed
client-secret reference; redirect URIs; scopes; PKCE and nonce policy; accepted signing algorithms;
and mappings from namespaced claims to Aimee tenant, principal, group, and role attributes. GitHub,
Entra ID, Okta, Keycloak, Auth0, or another conforming issuer may be configured through the same
contract; none is compiled in, privileged, or selected by a provider enum.

When `governance` is selected, issuer profiles are configurable from Aimee Control Plane's
governance UI and the equivalent CLI, environment/configuration, and non-web API surfaces. The UI
renders effective fields from the same descriptor/config metadata and stores secrets only through
`vault`; it never embeds a client secret in rendered config or browser state. With `control-web`
disabled, every OIDC operation remains available headlessly. With `governance` absent, issuer
profiles and every OIDC setting are absent from advertised GUI, CLI, environment catalog, config
schema, and API surfaces; accepted legacy input, if any, is migration-only and never advertised.

Core contracts remain product-neutral. Runtime and Control choose deployment topology and custody:
Runtime hosts per-user memory and enforcement instances; Control hosts multi-tenant shared-memory
instances, distributes policy artifacts consumed by core `execution-policy`, and projects the core
`audit` ledger through governance/visibility surfaces. Control does not own alternative memory,
policy, or audit contracts, and its dashboard does not own the audit ledger.
Control audit projections are deterministic read-only views of canonical ordered ledger events;
they cannot cache, reorder, omit, or rewrite events outside an explicitly authorized query filter.
Legacy KB policy artifacts are never trusted by rename alone: Control rebinds them to a verified
tenant and principal and reauthorizes them through core execution policy before distribution.

Canonical code, binaries, packages, docs, routes, and config use Runtime/Control names. Old
`aimee-server`, `aimee-kb`, server-registry, and kb-client names survive only through explicit,
bounded compatibility records. Descriptor aliases implement old names; the delivery proposal owns
their authority, retention, and expiry. New legacy-name uses fail CI.

## Web modules

`runtime-web` and `control-web` are independent optional modules and are enabled by default in their
product profiles. Each owns its GUI, assets, routes, listeners, background work, and dashboard. A
dashboard is inseparable from its GUI and has no independent module or enable key. Binding check 2
proves both the default-enabled state and independent omission behavior.

Inseparable means a dashboard has no descriptor, module document, enable key, standalone listener,
or routes outside its owning GUI's route prefix. Trigger, cron, and event-activation entrypoints
remain owned by `workflows`; a GUI may manage them but may not register a parallel scheduler.

The only web lifecycle controls are `runtime.web.enabled` and `control.web.enabled`. Other optional
modules may declare their own runtime-toggle control through the descriptor contract. Web controls
are evaluated at startup and are restart-class settings; changing them does not restart the process
automatically. When false, the owning web module does not register, load assets, bind listeners,
expose routes, or start background work. Both products remain fully operable through CLI,
environment variables, configuration files, and non-web APIs.

Legacy web-route aliases are owned by their web module and exist only while that module is selected
and enabled. When it is disabled, the generic gateway returns typed `capability_absent` for a known
legacy web capability (or the normal unknown-route response after alias expiry); it does not serve,
forward, or re-register the web route. Non-web product/config aliases remain available according to
their compatibility records.

Externally, disabled and unknown web routes have identical status, body shape, timing class, and
headers; `capability_absent` is an internal audit reason only. Core has no HTML renderer or fallback
dashboard handler. Runtime disablement may leave a selected module's objects in the build but
forbids loading/registration/listeners/routes/assets/jobs/metrics or symbol invocation; build-profile
omission separately proves those objects and symbols are absent.

Changing a web setting never reloads or restarts the current process. On an operator-controlled
cold restart, the new process validates persisted config before registering any listener and either
starts once with the new setting or fails closed; it never falls back to the old effective config.

## Truthful effective configuration

The module/build proposal owns descriptor fields for config-key ownership and production read sites.
This proposal owns the **effective catalog**: the activation-filtered projection of those declarations
rendered as CLI schema, API schema, both GUIs, defaults, and profile snapshots. A setting is
advertised only when its owner is selected, its activation state permits it, and a non-test
production read proves it has effect.

Absent modules expose no settings. An optional module whose descriptor declares runtime-toggle
support may expose only its lifecycle enable key while disabled; operational settings appear after
activation. The suite and descriptor contract define that required modules have no enable control;
this proposal tests that invariant in the rendered surfaces. Legacy persisted keys may remain
accepted without being advertised only through the delivery proposal's compatibility record and
retention window. Unowned, unread, stale, or wrong-activation keys enter deletion/deprecation.

## Binding checks

```yaml acceptance
- {id: 1, tier: integration, check: "scripts/test_product_boundary_rename.sh --canonical-runtime aimee-runtime --canonical-control aimee-control --runtime-opaque-single-principal-handle --forbid-principal-collections-selectors-replacement --control-multi-tenant --governance-optional --require-core-local-auth-multi-tenant --require-governance-surfaces-absent-from-cli-env-config-api-web --require-audit-source-core-ledger-when-governance-absent --require-tenant-principal-policy-reauthorization --audit-projection-deterministic-read-only --compatibility-records --forbid-new-legacy-names"}
- {id: 2, tier: integration, check: "scripts/test_web_module_lifecycle.sh --modules runtime-web,control-web --default-enabled --independent-startup-disable --forbid-dashboard-descriptors --forbid-dashboard-module-docs --forbid-dashboard-enable-keys --forbid-dashboard-listeners --require-dashboard-routes-under-gui --forbid-parallel-schedulers --legacy-web-aliases-module-owned --disabled-route-indistinguishable-from-unknown --forbid-core-html-fallback --forbid-disabled-load-register-listen-route-asset-job-metric-symbol-invocation --require-omitted-build-link-symbol-absence --forbid-gateway-web-forwarding --no-auto-restart-reload --cold-restart-config-before-listeners --forbid-old-config-fallback --require-headless-cli-env-config-api"}
- {id: 3, tier: integration, check: "scripts/check_effective_config_surface.sh --profiles core,runtime,control,full --full-minus-one-every-optional --api-exact --gui-exact --hide-absent --disabled-enable-only --fail-required-enable-control --fail-unread --fail-unowned --fail-stale --fail-wrong-activation --check-legacy-migrations"}
- {id: 4, tier: mechanical, check: "scripts/compare_surface_baseline.sh --config --profiles core,runtime,control,full --separate-persisted-input-from-advertised"}
```
