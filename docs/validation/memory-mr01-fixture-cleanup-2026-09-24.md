# MR-01: disposable fixture network cleanup

Repeated Compose Vault preparation failures occurred before application checks.
The [redacted bootstrap diagnostic](memory-mr01-fixture-cleanup-2026-09-24/bootstrap-diagnostic.json)
identified exhaustion of Docker's default non-overlapping IPv4 pools in CT109.
Nineteen unused Server networks remained from completed enrolled fixtures.
Server teardown ran while an independently owned KB was still attached; removing
the KB later left those now-empty Server networks behind.

Only empty networks matching recorded task projects with no remaining containers
were removed. The harness now disconnects an enrolled task KB peer from its own
Server network before Compose teardown. It does not change application networking
or the released CT100 installation. The parity harness records network cleanup;
its [completed current run](memory-mr01-decision-parity-2026-09-24/current/cleanup-networks.json)
leaves neither project's networks behind after 54 successful application checks.
