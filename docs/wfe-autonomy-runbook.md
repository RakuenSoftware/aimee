# Runbook: the autonomous WFE lifecycle

The autonomous Work-Item Framing Engine (WFE) takes a single human-authored work item from intake to a merged pull request without any manual intervention between stages: it parses the scope, drafts and ratifies a plan against acceptance criteria, gates the work behind a roundtable sanity check, drives implementation as a sequence of atomic per-slice edits, and only opens a PR once every slice has been verified end-to-end. The marker fix-validate-1784468081 anchors this runbook to the slice that produced it so downstream tooling can prove provenance.

The end-to-end pipeline is:

- proposal intake
- plan
- roundtable gate
- implement per-slice
- verify
- PR open
