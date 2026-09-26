# MR-10 functional evidence

Final code: `f6115607c`. The final deployed replay is the `provider-v4`
run; earlier failed provider-positive-control and retired-key attempts are kept
as evidence, not counted as passing release gates.

- `full-race-export.log`: complete PostgreSQL memory race and exported owner
  tests on `052d7520f`.
- `final-clock-precision.log`: focused PostgreSQL/race tests after the final
  microsecond precision correction.
- `lint-initial.log`, `lint-corrected.log`: all 77 checks exercised, with the two
  registration failures repaired and the affected checks rerun successfully.
- `*-checks.json`: individual deployed assertions; `*-provider-*.json`: exact
  provider-payload hashes, synthetic fixture-presence booleans and receipt
  commitments. No raw provider prompt or production memory is included.
- `live.py.txt`, `provider.py.txt`, `upgrade.py.txt`, `final-audit.py.txt`: frozen
  final harnesses. They target the isolated CT109 canary, preserve its volumes,
  restore the provider roster/configuration and disable optional policies.

The final provider fixture uses a fixed 8,192-token legacy recall allocation in
both arms. This is a synthetic functional test; it is not a task-quality,
false-exclusion-rate, production performance or billing measurement. The
transient classification applies only to synthetic task-state records and two
exact-version fixture overrides; no fitted production policy is enabled.

See [the validation report](../memory-mr10-horizons-2026-09-26.md) for acceptance
scope, limitations and production status. `SHA256SUMS` covers these artifacts.
