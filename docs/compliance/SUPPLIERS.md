# Supplier and subprocessor register

The operator completes this register for services actually enabled. “Optional” means Aimee does not
send data there until configured; it does not remove the operator's due-diligence duty.

| Supplier class | Examples in source | Data/purpose | Enabled by default | Required review |
| --- | --- | --- | --- | --- |
| source/release hosting | GitHub/GHCR | source, CI logs, release artifacts | repository-dependent | access, retention, region, incident terms |
| model providers | configured OpenAI-compatible/Bedrock endpoints | prompts and selected context | no | DPA, training use, retention, region, subprocessors |
| model/package hosting | Hugging Face, npm, Go proxy, OS package repos | model/package identifiers and request metadata | build-time | provenance, integrity, availability, licensing |
| identity providers | configured OIDC issuer | identifiers, claims, login metadata | no | issuer trust, MFA, logging, lifecycle |
| telemetry exporters | configured OTLP endpoint | bounded operational telemetry | no | field inventory, redaction, retention, access |
| MCP/other integrations | operator-installed endpoints | tool-dependent content and credentials | no | package digest, destination, scopes, data flow |

Before enablement record legal entity, service owner, contract/DPA, purpose, fields, regions,
subprocessors, security review, breach SLA, deletion/export support, continuity, review date, and
exit plan. Reassess annually and on material change.
