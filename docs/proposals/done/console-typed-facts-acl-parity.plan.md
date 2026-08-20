# Console typed-facts ACL parity correction

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered; plan and frozen-diff reviews converged in jobs 8985 and 8986.
- **Scope:** restore the existing Go/C console-admin containment invariant.

The C `CONSOLE_ADMIN_ACL` already authorizes the three exact typed-facts console routes, while
the Go `consoleAdminACL` omits them. As a result, the existing `TestACLNoDriftWithC` fails on
`testing` and the browser console cannot reach already-authorized typed-facts handlers.

Add exactly `GET /v1/console/typed_facts`, `POST /v1/console/typed_facts/config`, and
`POST /v1/console/typed_facts/relation` to the Go mirror. Do not add patterns, modify the C ACL,
change credentials, or alter handler authorization. Verify the drift lock, deny-by-default tests,
Go race tests, and an adversarial focused roundtable. Merge this correction independently before
P5-D1 so that P5-D1 can keep the pre-existing console-admin allowlist unchanged.

## Outcome

The Go mirror now contains exactly the three literal method/path pairs already present in C. The
existing drift lock, deny-by-default suite, normal Go tests, and race tests pass. No C ACL,
credential, route matcher, or handler authorization changed.
