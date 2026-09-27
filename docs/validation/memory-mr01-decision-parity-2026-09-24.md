# MR-01/A7: equivalent decisions in actual Server and KB processes

Two independently deployed application/store/embedder stacks, enrolled through
the normal Vault-backed connection flow, pass [50 checks](memory-mr01-decision-parity-2026-09-24/checks.json)
for ten equivalent fixtures: current, expired, superseded, archived, quarantined,
deleted, revoked, rejected, retired and unknown lifecycle. Both processes run
application candidate `7eb4cf3b1`. The test finished with exit zero and removed
both disposable stacks.

For each fixture, HTTP get and the user-authorized validity diagnostic agree.
Excluded get responses must be `404/not_found` with no memory payload. Each
diagnostic retains its exact local owner, record ID and revision; the current
record's diagnostic version equals the serving version. The normalized
[domain decisions](memory-mr01-decision-parity-2026-09-24/domain-decisions.json)
match across Server and KB for policy, mode, eligibility, reason codes, lifecycle,
temporal applicability, evidence state and authority class. Observation clocks
and owner identities remain local and are deliberately not equated.

[Six actual container identities](memory-mr01-decision-parity-2026-09-24/image-identities.json)
are retained. The separately attested [1714-check deployment matrix](memory-mr01-serving-scope-2026-09-24.md)
provides module outage/native refusal and recovery evidence for the same image.
The common-state fixture only compares semantics represented by both stores;
shared future/suppression/scope cases remain covered by the separate common
fixture. This does not claim personal belief-time reconstruction, future-time
schema support or overall MR-01 completion. A subsequent implicit-audience repair
has its own pending candidate validation.

The [current candidate run](memory-mr01-decision-parity-2026-09-24/current/checks.json)
passes 54/54 application checks on `377c27b67`. Four additional checks reuse the
same lifecycle population for actual Server and KB list/search routes. The
runner exits zero; [cleanup verification](memory-mr01-decision-parity-2026-09-24/current/cleanup-networks.json)
confirms both projects leave no networks. An initial extension incorrectly sent
search a query string and limit 64; the corrected fixture uses the advertised
keywords array and limit 32. This was a harness request error, not a serving failure.
