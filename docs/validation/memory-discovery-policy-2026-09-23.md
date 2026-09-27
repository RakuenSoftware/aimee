# Incomplete discovery command classification — 2026-09-23

The execution-policy classifier sliced tokens starting at index two for every
`grep`, `rg` and `ripgrep` command. A bare tool name has one token and panicked
the policy handler. The [before regression](memory-discovery-policy-2026-09-23/before.txt)
reproduces that crash.

Argument-free search invocations now reach baseline operator policy without being
classified as repository discovery. They have no search target. Actual discovery
and specific-file behavior remain unchanged. Handler tests verify both the
unrestricted case and a matching operator prohibition; the latter remains denied.
The [execution-policy race suite](memory-discovery-policy-2026-09-23/go-race.txt)
passes. No hook configuration, fallback permission or task allowance changes.

This is an MR-07 classifier robustness repair. Authenticated task contracts,
shared budgets and starvation recovery remain open. It does not implement or
certify those gates.
