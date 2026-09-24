# MR-01: bind ordinary serving to verified credential scope

A restricted-role regression reproduced a cross-project read through the Go data
owner: a caller verified for project `visible` could request project `hidden`
and receive its record. The transaction settings previously followed the request;
only the validity diagnostic compared them with the verified caller scope.
The [failing replay](memory-mr01-serving-scope-2026-09-24/before-race.txt)
records the synthetic fixture disclosure.

The data owner now binds requests to the host-verified project/workspace scope
before opening the transaction. Omitted scope inherits that restriction;
`include_all` cannot widen it; conflicting scope or audience fields are refused.
Named deployment service credentials retain their existing data-plane access.
This does not grant human authority or administrative capabilities.

The native KB command envelope now carries verified scope for credentials that
have no human actor, using a credential principal and retaining false user
authority. Previously that scope was omitted unless an actor existed. The C
change transports credential identity; Go owns the memory scope decision.

The [targeted race replay](memory-mr01-serving-scope-2026-09-24/targeted-race.txt)
passes. The first full suite exposed an older diagnostic fixture that deliberately
used a different verified scope from its requested project. That fixture now
uses a matching authorized caller for successful trace checks and explicitly
asserts refusal for a mismatched caller. The [full race suite and export](memory-mr01-serving-scope-2026-09-24/full-race-export.txt)
pass (218.545 seconds for the race suite). Native KB adapter compilation and
ownership, C-boundary, module-bus, descriptor, documentation and proposal-link
guards pass. The new scoped-credential HTTP harness still needs execution against
the candidate image. No schema changes are introduced.

This closes a confirmed serving admission defect, not all MR-01 acceptance.
Internal callers without a credential scope, privileged cross-project admission,
remaining native data-stage transports, and final provider-release races still
need separate review. The released CT100 installation remains on 0.4.5; draft
validation uses CT109.
