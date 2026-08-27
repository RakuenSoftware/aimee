# Control and evidence register

Live GitHub settings are exported with `scripts/collect-live-governance-evidence.sh`. The collector
uses GET-only API calls, requires an offline signing key, content-addresses every response, and
refuses an empty ruleset or branch protection without an approving-review requirement. Run it at
least quarterly and after every ruleset, environment, or access change; retain the signed bundle
for 18 months. A source-only assurance bundle is never evidence that these live controls operated.

| Control | Accountable owner | Implementation/evidence | Cadence | Independent review | Evidence retention |
| --- | --- | --- | --- | --- | --- |
| secure change approval | Engineering Governance | CODEOWNERS, protected-branch export, PR review | each change | non-author | 2 years |
| dependency and supply chain | Release Engineering | pinned actions, govulncheck/npm audit, SBOM, provenance, signatures | each build | release approver | 2 years |
| access control | Identity | route/capability negative tests, access-review record | quarterly and each release | Security | 2 years |
| tenant isolation | Data Authorization | two-user/two-project negative matrix and RLS tests | each release | Security | 2 years |
| audit integrity/completeness | Audit | WORM verification, checkpoint/witness, event reconciliation | continuous/monthly review | Security | 2 years |
| vulnerability response | Security | private advisory, SLA log, remediation release | each report | incident commander | 3 years |
| incident response | Security | exercise/incident record and corrective actions | annual/each incident | executive owner | 3 years |
| backup and recovery | Platform | encrypted backup and restore-test report | quarterly | service owner | 2 years |
| privacy lifecycle | Data & Privacy | inventory, retention run, erasure record, legal holds | monthly/each request | Privacy reviewer | 3 years |
| supplier management | Security & Compliance | supplier register and reassessment | annual/change | service owner | 3 years |

Exceptions require an owner, control/asset, rationale, residual risk, compensating control,
approver distinct from the requester, issue date, expiry no longer than 90 days, and linked evidence.
Expired exceptions block release. `scripts/generate-assurance-evidence.sh` creates the source-side
bundle; live platform exports and signed reviewer records are added by the operator.
