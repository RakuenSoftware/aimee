# Incident response

The incident commander owns coordination; Security owns containment and evidence; the affected
service owner owns remediation; Communications owns required notices. One person may hold several
roles in a small deployment, but the incident log records each role and approval.

1. Triage: create a restricted incident record, timestamp the report, classify affected releases,
   data, tenants, credentials, and safety impact, and preserve volatile evidence.
2. Contain: revoke exposed identities, disable autonomous/egress paths, isolate affected services,
   preserve WORM anchors and logs, and avoid destructive cleanup until evidence is copied.
3. Eradicate: identify root cause and blast radius, patch through normal review, rotate custody
   material, and verify no alternate persistence remains.
4. Recover: restore from a verified backup, validate WORM and database consistency, monitor the
   affected indicators, and obtain incident-commander approval before full service resumes.
5. Close: record timeline, impact, decisions, notifications, lessons, corrective owners/dates, and
   a follow-up validation. Run a tabletop exercise at least annually and after material redesign.

Severity is Critical when active compromise, cross-tenant exposure, release-key compromise, or
tamper-evidence loss is plausible; High for material confidentiality/integrity/availability loss;
Medium/Low for bounded or defense-in-depth impact. Critical incidents page the owner immediately;
High incidents are acknowledged within four hours; other severities within one business day.

Preserve the source SHA, deployed manifests/digests, identity and access changes, WORM verification
output, checkpoints/witnesses, database snapshots, relevant logs, and a hash manifest. Evidence is
read-only, access-logged, encrypted, and retained under the legal-hold decision in
[Data governance](DATA_GOVERNANCE.md).
