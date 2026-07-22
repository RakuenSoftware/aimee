# WFE Autonomy Runbook

The WFE (Workflow Engine) autonomous lifecycle turns a written proposal into a
merged change with no operator in the loop. When a proposal is submitted it is
intake-scoped into a work item, planned, optionally reviewed at a roundtable
gate, then implemented in slices, verified, and finally opened as a PR — all
under the server-side `build` workflow. Human gates (e.g. the roundtable and
the PR-open step) are the only points that may pause the run.

## Pipeline stages

- proposal intake
- plan
- roundtable gate
- implement (per-slice)
- verify
- PR open

## Where to look

For a live trace of any work item, read its server-pushed event stream via the
canonical endpoint: `GET /v1/runs/{id}/events`. The same stream backs the
Workflows tab in the webchat UI.
