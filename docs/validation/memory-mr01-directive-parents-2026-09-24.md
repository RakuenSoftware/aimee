# MR-01: directive recall requires eligible parents

Open, unexpired directives previously entered recall even when their referenced
memories were future-dated, expired, suppressed, superseded, archived,
quarantined, deleted, revoked, missing, or outside the requested audience.
Higher-priority invalid questions could consume the entire result limit.

The shared Go owner now requires every nonzero memory parent to satisfy current
memory eligibility before matching or fallback limits. This includes both input
memories and the resolution memory, and uses the generated-card input fence.
Questions authored without memory parents remain supported. The briefing
adapter also applies the common audience parser. No schema change is needed.

The [packaged restricted-role reproduction](memory-mr01-directive-parents-2026-09-24/before-race.txt)
shows both matched and fallback disclosure. The [targeted repair test](memory-mr01-directive-parents-2026-09-24/after-race.txt)
passes in 1.215 seconds. The extended regression checks all three parent
positions, mixed authorized/foreign inputs, missing inputs, and parentless
questions. The [HTTP reproduction](memory-mr01-directive-parents-2026-09-24/http-before.json)
on `b866f1ac0` fails the first directive check after scoped statistics pass.
The [full memory race suite and export build](memory-mr01-directive-parents-2026-09-24/full-race-export.txt)
pass in 211.766 and 4.784 seconds. Boundary, ownership, descriptor, inventory,
proposal-link and documentation guards pass. Corrected process validation is pending.

This repair covers serving selection. It does not certify operator directive
lists, mutation authorization, durable source observations for directive text,
or the final transport release boundary. MR-01 remains open.
