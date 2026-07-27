# KB console

The KB console, called `control-web` in newer trees, is a separate administration client for
`aimee-kb`. It does not reuse the runtime browser's PAM trust or server routes.

## Boundary

- OIDC identifies an administrator; an explicit presence flag controls break-glass login.
- Sessions are bound to issuer and subject.
- CSRF protects mutations.
- A deny-by-default allowlist limits the proxy to console-admin KB routes.
- The KB repeats the allowlist and remains authoritative.
- Administration actions write a console audit record and the owning KB audit path.

The console credential file is private and read-only to the process. The browser never receives the
KB service bearer.

## Deploy

Place the console near the KB, expose only its HTTPS listener, and keep the KB origin private. Pin
OIDC issuer, audience, signing algorithms, JWKS behavior, and the admin claim. Do not enable
break-glass permanently.

## Verify

- unauthenticated and non-admin users cannot reach proxy routes;
- cross-site mutation fails CSRF;
- a route absent from either allowlist is denied;
- session reuse under another issuer/subject fails;
- OIDC key rotation works without accepting an unexpected issuer;
- break-glass is off after recovery;
- audit rows identify the administrator and operation.

Source builds live under `control-web/` in current trees and `kb-console/` in older checkouts.
