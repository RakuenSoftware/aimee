# MR-01: preserve shared visibility when binding verified scope

Review of the credential-scope repair found that an inherited scope also set the
internal exact-scope query flag. Ordinary audience list/search then lost allowed
global records. A [regression](memory-mr01-implicit-audience-2026-09-24/before-race.txt)
reproduces that loss without admitting any foreign project.

The data owner now captures whether the request explicitly selected a scope
before binding the host-verified restriction. Foreign projects remain excluded
and `include_all` remains narrowed; implicit audience reads retain the existing
shared/global visibility policy. Explicit exact-scope reads stay exact. The
[targeted race replay](memory-mr01-implicit-audience-2026-09-24/targeted-race.txt)
passes, including the prior foreign-project refusal regression. The [full race suite and export](memory-mr01-implicit-audience-2026-09-24/full-race-export.txt)
pass (212.213 seconds for race, 5.309 seconds for export). The
[HTTP regression on the prior image](memory-mr01-implicit-audience-2026-09-24/http-before.json)
also reproduces the global-visibility loss. [Updated scoped HTTP evidence](memory-mr01-implicit-audience-2026-09-24/http-after/checks.json)
passes all 16 checks and 128 concurrent requests on candidate `7099bf52c`. No schema or native policy changes
are introduced.
