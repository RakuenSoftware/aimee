# Console typed-facts ACL parity correction

- **State:** plan review pending.
- **Scope:** restore the existing Go/C console-admin containment invariant.

The C `CONSOLE_ADMIN_ACL` already authorizes the three exact typed-facts console routes, while
the Go `consoleAdminACL` omits them. As a result, the existing `TestACLNoDriftWithC` fails on
`testing` and the browser console cannot reach already-authorized typed-facts handlers.

Add exactly `GET /v1/console/typed_facts`, `POST /v1/console/typed_facts/config`, and
`POST /v1/console/typed_facts/relation` to the Go mirror. Do not add patterns, modify the C ACL,
change credentials, or alter handler authorization. Verify the drift lock, deny-by-default tests,
Go race tests, and an adversarial focused roundtable. Merge this correction independently before
P5-D1 so that P5-D1 can keep the pre-existing console-admin allowlist unchanged.
