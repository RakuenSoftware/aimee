# MR-01 final closeout — 2026-09-24

MR-01 is complete against its seven unchanged acceptance gates. The frozen
123-clause program inventory is unchanged; MR-02 through MR-18 remain pending.

## Behavior

Both Go memory placements apply authenticated scope, lifecycle and half-open
time eligibility before ranking, traversal, aggregation and context release.
The host supplies EligibilityContext (principal, audience, workspace/project,
purpose, mode, time anchors, policy and checked source generations). The storage
transaction captures one clock, including private recall and optional vectors.
Unsupported temporal modes fail explicitly; retained historical reads still
exclude erased, revoked and foreign content. Evidence and authority that cannot
be established remain unknown.

Persistent context channels carry observed root and supporting-parent revisions,
including private active context, learning observations/proposals, relations and
hard-rule collection generations. Revalidation and durable send guards protect
actual provider release; pending completion never becomes permission to mutate
merely because a deadline elapsed. Explicit completion and bounded background
recovery release protection. The earlier decision remains in the receipt history.
Ephemeral working context is request input, not a stored revocation source.

## Final validation

Application commit `a21288b28` was built once as
`sha256:9b82217b95f27eee0d34fa7eb3a7f475f14aa34aa58abdda22ae5fefe6256e89`.
The immutable harness is `08e95496f`; its only subsequent fixture adjustment
allocates space for mandatory hard-rule source metadata while retaining optional
packing and protected-overflow assertions. All processes exited zero:

| Final process | Passed checks |
|---|---:|
| Common lifecycle/scope HTTP fixture | 201 |
| Matched Server/KB decisions | 58 |
| T2 deployment, restart, outage and provider release | 1,097 |
| T3 deployment, restart, outage and provider release | 617 |
| Total | 1,973 |

[Raw receipts, image identities and process exits](memory-mr01-final-evidence-2026-09-24/process-exits.json)
are retained alongside each fixture's checks. Nine container image identities
were captured; all three application owners used the image above and their actual
provider caps were 32,768 bytes. [Cleanup](memory-mr01-final-evidence-2026-09-24/cleanup.json)
confirms no fixture-owned containers or networks remain.

The complete PostgreSQL memory race suite passed in 286.516 seconds and exported
owner check in 4.816 seconds ([output](memory-mr01-private-closeout-2026-09-24/combined-race-export.txt)).
The final [native ingress suite](memory-mr01-final-evidence-2026-09-24/native-ingress.txt)
and [all 77 lint gates](memory-mr01-final-evidence-2026-09-24/lint.txt) passed.
Post-image production C edits are formatting only, with
[identical lexical tokens](memory-mr01-final-evidence-2026-09-24/formatting-token-check.json).
Other closeout edits update test temporary paths, generated ledgers, schema
checker classification and documentation. Append-only migrations 34 and 35 are
unchanged; their two intentionally repeated guard declarations must have equal
SQL tokens, and a third declaration or divergent definition still fails.

The common HTTP fixture's 144 timed calls measured 141.912 ms median and
214.972 ms P95 ([samples summary](memory-mr01-final-evidence-2026-09-24/http-latency.json)).
This is a synthetic CT109 fixture concurrent with parity validation, not a general
performance benchmark or MR-18 certification. Exact-set and rejection assertions
cover lifecycle, scope and historical exclusions.

## Gate traceability and released installation

The [seven-gate checklist](../proposals/pending/memory-reliability-01-closeout.md)
and [serving inventory](../proposals/pending/memory-reliability-01-serving-inventory.md)
map the final process fixtures and dedicated adversarial regressions to the
original contract. All draft application/schema validation ran on disposable
CT109. [CT100 health](memory-mr01-final-evidence-2026-09-24/production-045-health.txt)
confirms server, database and embedder remain healthy on released 0.4.5 after the
0.4.4 upgrade. The paired local thinclient successfully performs the registered
CLI repository operations. PreToolUse remains temporarily disabled as requested.

This closes MR-01 implementation and validation in draft PR #2990. It does not
merge that PR, deploy its draft schema to CT100, or certify later proposals.
