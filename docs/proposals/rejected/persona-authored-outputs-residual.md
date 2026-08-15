# Persona-authored outputs: residual work

- **State:** REJECTED — mixed C/Go ownership cannot be completed as one Go proposal; archived
  2026-08-15.

**Archived parents:** [`persona-authored-outputs.md`](../done/persona-authored-outputs.md) and
[`persona-authored-outputs.plan.md`](../done/persona-authored-outputs.plan.md)

## Decision

Rejected under the Go-or-rejected implementation policy. This residual combines boundaries with
different owners: role-permission resolution and the permission-to-tool clamp now live in
`server-go/modules/delegates`, while persona composition, authored workflow blocks, and persona
configuration remain in C. Completing the umbrella as one Go change would either duplicate those C
owners or reach across their module boundaries.

The delivered Go permission foundation remains authoritative. This rejection does not say persona
voice or authored-output provenance is unsound; any remaining work must be split into owner-specific
Go proposals after the corresponding persona, workflow, and artifact boundaries have Go owners.

## Remaining deliverables

- Define the persona permission model and enforce it at every authored-output boundary.
- Apply persona voice and commit-style preferences without weakening policy or provenance.
- Bind commits, review artifacts, and other authored outputs to the effective persona and actor.
- Resolve server behavior when no persona is supplied and document compatibility.
- Add denial, impersonation, provenance, and migration tests.
