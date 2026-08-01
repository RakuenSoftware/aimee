# Browser workspace

The browser is a client of `aimee-server`, `aimee-wfe`, and `aimee-kb`. It owns login and UI session
state; it does not own product data.

Login is a **local PAM account** in the `aimee-webchat` group, checked through the `aimee` PAM
service, the same stack the KB's `/v1/identity/login/pam` uses. Accounts outside that group are
never dashboard logins, so the container's own system users cannot sign in. An unavailable PAM stack
is reported as such rather than as a wrong password.

Which flow applies follows the connected KB's `/v1/identity/auth-mode`. Under `oidc` the identity
provider owns accounts and local account management is refused; the wizard's account step disappears.
Dashboard login itself remains PAM in this release, with the OIDC flow arriving in 0.4.0. Any failure
to reach the KB resolves to PAM, which is the mode with a local answer.

## Pages

- **Chat:** server-owned conversations and tools.
- **Dashboard:** server-incurred agent, token, cost, latency, cache, guardrail, and readiness metrics.
- **Projects:** git accounts, clones, workspaces, and per-page project selection.
- **Agents / Roundtables:** roster, probes, role coverage, presets, and seats.
- **Edit Workflows:** visual definition editor.
- **Workflow Actions:** start and operate durable runs.
- **Graph:** code and relationship exploration.
- **Logs:** action/audit views for the selected service.
- **Settings:** allowlisted typed configuration.
- **Editor:** per-user VS Code through the authenticated proxy.

Each page keeps its own project selection. Selecting a project does not grant authority; every server
route checks the current principal and scope.

## Dashboard metrics

Panels are based on work the server incurred: delegations, role/agent success, tokens, cost,
latency, cache efficiency, provider mix, tool activity, guardrail actions, sessions, and readiness.
Users can hide and reorder panels; layout is browser preference, not server state.

The Logs page is separate because audit rows and operational logs have different retention and
access rules. The KB has its own internal administration surface.

## Settings

The Settings page edits the same field descriptors as `aimee config`. It cannot expose secrets or a
raw YAML editor. See [Settings](SETTINGS.md).

## Managed deploy

The setup wizard can start the KB container when the server has the Docker socket. That
is Docker-host authority. Use the split stack when the browser must not control deployment.

## Security

Browser requests need login, secure cookies, CSRF checks for mutation, and principal propagation.
The proxy uses deny-by-default route families. Credentials stay in the server vault.

See [Web git security](WEBCHAT_GIT_SECURITY.md), [Workflow Actions](WORKFLOW_ACTIONS.md), and
[VS Code](VSCODE.md).
