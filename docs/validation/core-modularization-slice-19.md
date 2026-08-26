# Core modularization slice 19: control-plane, governance, and web documentation

## Diff scope precondition

The slice-start commit is `22a5f63f8f2fd3adbaba4215859fa77a4a724985`. Allowed close paths are the
three module documents, this validation record, the documentation-status partition, and the cleanup
ledger. Production source, descriptors, build/package/service files, schemas, GUI, generated artifacts,
checkers, and tests are excluded.

## Outcome and method

This slice promotes the final three documentation debts: `control-web`, `governance`, and `runtime-web`.
Every canonical module now has an individual substantive document. Inspection covered descriptors,
physical inventories, proposal contracts, frontend route shells, Go process entrypoints/configuration,
container and compose lifecycle, dashboard implementations, OIDC/JWKS/identity routes and stores,
management tokens, response governance, tests, and compatibility names.

Aimee memory was queried first and returned only generic recent episode facts, so nearby source inspection
supplied the evidence. Static references prove compilation and reachability, not deployed liveness.
Descriptor claims and approved target contracts are reported separately from current behavior; this
docs-only slice does not repair their contradictions.

## Inventory and activation evidence

### Control web

`src/modules/control-web` contains only `module.yaml`: four dependencies, `enabled_by_default: true`, and
runtime-toggle support. Current implementation is distributed across `kb-console`,
`frontend/src/console`, `Dockerfile.kb-console`, `compose.yaml`, KB console routes, and tests. The Go
executable describes itself as default-off, requires a console-admin credential, and the compose service
is gated by the `console` profile. This contradicts the target default-on module profile. Current OIDC
config is fetched from `/v1/config/oidc` once at startup when a local file did not configure it.

The React `ConsoleApp` always includes Dashboard, Accounts, and Governance navigation. Dashboard is
already a page of the GUI, not a separately configured UI component. Governance visibility is currently
unconditional in the shell, so absent-module page/config filtering remains implementation debt.

### Runtime web

`src/modules/runtime-web` also contains only `module.yaml`, with the same four dependencies,
`enabled_by_default: true`, and runtime-toggle support. Current implementation spans `webchat`, the main
`frontend/src` SPA, `dashboard.c`, `dashboard_kb.c`, server dashboard/API routes, `Dockerfile.server`,
compose entrypoints, systemd, and tests. Ordinary `make all` includes `aimee-webchat`; containers can omit
it with `WITH_WEBCHAT=0` or suppress startup with `AIMEE_WEBCHAT_ENABLED=0`. These legacy controls are not
yet generated from `runtime.web.enabled` or the descriptor.

The SPA includes Dashboard in the same route shell as Chat and capability pages. Legacy `aimee
dashboard` remains a standalone listener and therefore conflicts with the target inseparable-dashboard
contract. GitHub/GitLab OAuth fields in Runtime compose/web code are Git-provider integration, not
organizational OIDC governance.

### Governance

`src/modules/governance` contains the descriptor and one response-stage implementation. The descriptor
declares eleven required dependencies, `enabled_by_default: false`, and no runtime toggle. The response
stage uses caller-resolved `modules.governance`, but its deprecated `AIMEE_STAGE_GOVERNANCE` fallback is
default-on and covers only response tool-use policing. Provider-neutral OIDC behavior is separately
distributed across `src/kb/auth_oidc.c`, identity/JWKS code, DB2 enrollment/grant tables,
`/v1/config/oidc`, `kb-console`, and management-token code. Descriptor-level selection does not yet omit
those objects, stores, routes, or settings.

The current OIDC configuration shape, issuer, audience, JWKS URL, admin claim, and admin values, is
provider-neutral but partial. No provider enum was found. The target named issuer-profile contract adds
discovery/explicit endpoints, vault secret references, redirects, scopes, PKCE/nonce, accepted
algorithms, and namespaced claim mapping. Git/SSH/SSHSIG remain core; governance may consume their
verified evidence but does not own verification or a repository adapter.

## Dependency reconciliation

| Module/dependency | Evidence classification | Boundary |
|---|---|---|
| control-web/runtime-web → config | declared; distributed current settings observed | generated effective catalog and startup controls are target gaps |
| control-web/runtime-web → gateway/protocols | declared; proxy/API routes observed | typed API transport, never core HTML fallback |
| control-web/runtime-web → module-runtime | descriptor-only at canonical lifecycle boundary | legacy build/env/compose controls remain |
| governance → audit/config/vault/execution-policy | declared and distributed behavior observed | canonical ledger/config/custody/enforcement remain core |
| governance → delegates/ir/routing/tools | declared; partial organizational consumers observed | governance consumes identities/records/providers/dispatch without replacing owners |
| governance → gateway/protocols | declared; OIDC/control routes observed | typed control admission/transport |
| governance → module-runtime | descriptor selection not enforced across distributed code | full-minus-one omission remains absent |

No declared edge is classified stale from static evidence. The primary gaps are physical ownership,
selection/lifecycle enforcement, and effective-surface generation.

## Ownership and liveness matrix

| Capability | Owner | Current provider/evidence | State / ambiguity |
|---|---|---|---|
| Control GUI and dashboard | control-web | `kb-console`, `frontend/src/console` | reachable/tested; descriptor-only owner; current default-off conflicts |
| Runtime GUI and dashboard | runtime-web | `webchat`, `frontend/src`, legacy dashboard/server code | reachable/tested/default-on container; lifecycle fragmented |
| GUI-effective settings | config + owning module metadata | static Settings/Accounts pages and config APIs | active-provider filtering not enforced |
| Organizational OIDC | governance | KB auth/JWKS/config/routes and console | provider-neutral seed implemented; selection/complete profile absent |
| Browser/session auth | owning web module | Go console/webchat auth and session stores | implemented/tested; not governance policy ownership |
| Core action enforcement | execution-policy | response policing and action gates | required core; response-stage split needs reconciliation |
| SSH/SSHSIG verification | Git/protocol/core trust owners | Git/signature paths and attestations | core evidence source; not OIDC or repository adapter |
| Audit ledger | audit | legacy/WORM providers | core canonical source; governance/web only project views |

## Target lifecycle and surface truth

`control-web` and `runtime-web` are independently selectable, optional, and enabled by default in their
product profiles. `control.web.enabled` and `runtime.web.enabled` are startup-evaluated restart-class
settings: changing them never restarts or reloads a process automatically. Disabled modules do not load,
register, listen, serve routes/assets, start jobs, emit metrics, or receive symbol calls. Omitted builds
also lack their objects and symbols. Both products retain CLI, environment/config-file, and non-web API
operation.

Each dashboard is inseparable from its GUI. The target has no dashboard descriptor, document, setting,
listener, or routes outside its GUI prefix. GUI settings are activation-filtered projections: a setting
is advertised only when its owner is selected/active and a non-test production read proves effect.
Governance absent means organizational and OIDC settings/pages are absent; control-web absent does not
remove headless governance configuration.

## Overlap and cleanup findings

- Both target web directories are descriptor-only; existing application directories and broad legacy
  dashboard/server roots are `relocate` or `split-owner` candidates, not dead code.
- `AIMEE_WEBCHAT_ENABLED`, `WITH_WEBCHAT`, the compose `console` profile, and executable-local flags are
  compatibility/unification debt for descriptor-driven lifecycle generation.
- Standalone `aimee dashboard` and dashboard routes outside the owning GUI prefix conflict with the
  inseparable-dashboard target and require supported-surface compatibility analysis.
- Governance response policing overlaps core `execution-policy`; non-negotiable enforcement cannot move
  to an optional module. Organizational policy authoring/decisions may move while core application stays.
- Current OIDC code is provider-neutral but distributed and only partially models the target issuer
  profile. GitHub OAuth remains Git integration and is not an OIDC default.
- Unconditional GUI pages/static config fields can advertise inactive modules and need generated
  capability/effective-config filtering.

No `unreachable`, `superseded`, `configuration-only`, or `test-only` candidate meets the deletion
threshold. Source movement requires production journeys, consumers, profile residue, data custody,
tenant isolation, and compatibility evidence.

## Verification

- `python3 -I -S scripts/check_module_docs.py`
- `python3 -I scripts/tests/test_check_module_docs.py -v`
- `python3 -I -S scripts/check_module_source_ownership.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I -S scripts/refactor_baselines.py`
- `make -C src lint`
- close-time changed-path and diff-stat scope checks
- technical-writer review and exact final-diff roundtable approval
- feature-branch pull-request CI
