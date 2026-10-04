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

## Memory review boundaries

The typed-fact page invokes KB operator approve, reject, and undo actions. This is separate from
memory correction proposals, which retain a protected record while a model drafts a replacement.

The runtime browser's Memory Center now lists and reviews correction proposals for both
placements. Review checks the expected version, content digest and placement before accepting a
replacement. Personal corrections belong to Server and require explicit `store=user` on those
review operations. The control-web typed-fact page remains a separate operator interface.
See [Knowledge](KNOWLEDGE.md#corrections-and-history) and
[the implementation](../frontend/src/pages/Memory.tsx). The earlier
[review recommendation](reviews/agent-memory-atlas-2026-09-29.md#5-complete-the-user-visible-correction-workflow)
records the gap before this interface was added.

## Deploy

Place the console near the KB, expose only its HTTPS listener, and keep the KB origin private. Pin
OIDC issuer, audience, signing algorithms, JWKS behavior, and the admin claim. Do not enable
break-glass permanently.

For the shipped Compose topology, install the scoped credential in the mounted secrets directory:

```bash
mkdir -p control-web-secrets
sudo install -m 0600 -o 10001 -g 10001 /path/to/console.cred \
  control-web-secrets/console.cred
```

Override the host directory with `CONTROL_WEB_CRED_DIR`. The numeric ownership matches the
non-root `controlweb` user in the published image. When the file is absent, the optional console
idles without opening a listener and remains healthy; restart the service after provisioning it.

## Verify

- unauthenticated and non-admin users cannot reach proxy routes;
- cross-site mutation fails CSRF;
- a route absent from either allowlist is denied;
- session reuse under another issuer/subject fails;
- OIDC key rotation works without accepting an unexpected issuer;
- break-glass is off after recovery;
- audit rows identify the administrator and operation.

Source builds live under `control-web/` in current trees and `kb-console/` in older checkouts.

## Due decisions

Governance opens with the `revisit_due` operator queue. The revisit timestamp is shown
alongside each decision. **Close review** marks a reviewed decision `superseded`, so it
leaves the due queue. Use the status selector to inspect its retained history. These
reminders are operator work items and are not automatically injected into model context.
