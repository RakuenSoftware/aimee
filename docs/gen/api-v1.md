# aimee-kb API — v1

> Auto-generated from `api/openapi-v1.yaml` by `scripts/gen-api-docs.py`. Do not edit by hand; run `make docs-gen` to regenerate.

Total endpoints: 52

## Endpoints

### `POST /v1/actions/{action}`

Execute a versioned knowledge-service action

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `action` | path | yes | string |  |

Request body (`application/json`).

Responses:

- `200` — Action response
- `401` — Unauthorized
- `404` — Unknown action

### `GET /v1/artifacts/{id}`

Retrieve an artifact by UUID

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | string |  |

Responses:

- `200` — Artifact payload and citations
- `401` — Unauthorized
- `404` — Artifact not found

### `GET /v1/artifacts/{id}/links`

Retrieve outgoing links from an artifact

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | string |  |

Responses:

- `200` — Artifact links
- `401` — Unauthorized
- `404` — Artifact not found

### `GET /v1/capabilities`

Advertised capabilities

Returns the set of capability strings this aimee-kb instance supports.
Phase 1 always returns ["memory", "search", "index"].

Responses:

- `200` — Capability list
- `401` — Unauthorized

### `GET /v1/code/blast-radius`

Blast-radius computation for a file

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `project` | query | yes | string |  |
| `file_path` | query | no | string |  |

Responses:

- `200` — Blast radius
- `400` — Missing required parameters
- `401` — Unauthorized
- `404` — File not found in index

### `POST /v1/code/build`

Build a project knowledge index

Request body (`application/json`).

Responses:

- `200` — Build complete
- `400` — Missing required parameters
- `401` — Unauthorized
- `503` — Knowledge or vector store unavailable

### `GET /v1/code/callers`

Call sites for a symbol in the canonical code index

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `symbol` | query | yes | string |  |
| `project` | query | no | string |  |
| `max_results` | query | no | integer |  |

Responses:

- `200` — Caller results
- `400` — Missing symbol parameter
- `401` — Unauthorized
- `503` — Canonical index unavailable

### `GET /v1/code/cross-repo-deps`

Cross-repo dependency edges for a project (confidence-tiered, with evidence + version stamp), or the AMBIGUOUS review queue when status=ambiguous

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `project` | query | yes | string |  |
| `direction` | query | no | string (out, in, both) | Dependency direction: "out" = deps OF the project (default), "in" = repos that depend ON the project (reverse), "both" = union. Ignored when status=ambiguous. |
| `min_tier` | query | no | string (high, medium, tentative) | Minimum confidence tier to emit. Ignored when status=ambiguous. |
| `status` | query | no | string (ambiguous) | When "ambiguous", returns the AMBIGUOUS review queue for the project instead of edges (direction/min_tier are not applied in that mode). |
| `dry_run` | query | no | string (1, true) | Offline candidate inspection: emit every confidence band (down to LOW, overriding min_tier) plus the AMBIGUOUS candidates inline, and write nothing (no review-queue rows). The response carries dry_run:true and an additional `ambiguous` array alongside `deps`. |

Responses:

- `200` — Cross-repo dependency edges (or ambiguous review queue)
- `400` — Missing required parameters
- `401` — Unauthorized
- `413` — Response too large; narrow the query
- `503` — Canonical index unavailable

### `GET /v1/code/find`

Symbol/identifier lookup across the canonical index

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `identifier` | query | yes | string |  |
| `project` | query | no | string |  |
| `max_results` | query | no | integer |  |

Responses:

- `200` — Code find results
- `400` — Missing identifier parameter
- `401` — Unauthorized

### `GET /v1/code/project-stats`

Project-level canonical code index counts and language breakdown

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `project` | query | yes | string |  |

Responses:

- `200` — Project index statistics
- `400` — Missing required parameters
- `401` — Unauthorized
- `503` — Canonical index unavailable

### `GET /v1/code/projects`

List projects in the canonical code index

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `max_results` | query | no | integer |  |

Responses:

- `200` — Indexed projects
- `401` — Unauthorized
- `405` — Method not allowed
- `503` — Canonical index unavailable

### `POST /v1/code/repo-trust`

Set a registered repo's cross-repo trust (owner credential only)

Transactionally sets projects.trust, bumps cross_repo_meta.trust_epoch on a real transition, audits the change to cross_repo_trust_audit, and (on a change) recomputes the blocked_symbols frequency model. A scoped token is rejected with 403; the project must already exist.

Request body (`application/json`).

Responses:

- `200` — Trust applied (status, project, prior_trust, new_trust, changed)
- `400` — Missing project or invalid trust value
- `403` — Forbidden (requires the owner credential)
- `404` — No such project
- `405` — Method not allowed
- `503` — Knowledge service store unavailable

### `POST /v1/code/scan`

Request a canonical code index scan

Request body (`application/json`).

Responses:

- `202` — Scan accepted
- `401` — Unauthorized
- `405` — Method not allowed

### `GET /v1/code/search`

Full-text code search across indexed file contents

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `query` | query | yes | string |  |
| `project` | query | no | string |  |
| `max_results` | query | no | integer |  |

Responses:

- `200` — Code search results
- `400` — Missing query parameter
- `401` — Unauthorized
- `503` — Canonical index unavailable

### `GET /v1/code/structure`

Definitions for a file in the canonical code index

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `project` | query | yes | string |  |
| `file_path` | query | yes | string |  |
| `max_results` | query | no | integer |  |

Responses:

- `200` — File definitions
- `400` — Missing required parameters
- `401` — Unauthorized
- `503` — Canonical index unavailable

### `POST /v1/code/update`

Incrementally update a project knowledge index

Request body (`application/json`).

Responses:

- `200` — Update complete
- `400` — Missing required parameters
- `401` — Unauthorized
- `503` — Knowledge store unavailable

### `GET /v1/console/overview`

Web-console dashboard overview

Aggregated kb health/throughput for the aimee-kb web console dashboard.
Requires a console-admin credential (see the kb-web-console proposal).
S0 returns the envelope with an empty components list; S1 fills it with
an in-process telemetry fan-in.

Responses:

- `200` — Overview envelope
- `401` — Unauthorized
- `403` — Forbidden (credential not permitted for this route)

### `POST /v1/docs`

Upload a document for ingest

Accepts multipart/form-data with a required `file` part and an optional
`scope` field (default: global). Normalizes to markdown, stores in DB2,
and queues an extract_doc job. Returns existing doc_id on idempotent re-upload.

Request body (`multipart/form-data`).

Responses:

- `200` — Idempotent re-upload (existing doc_id returned)
- `201` — Document ingested
- `400` — Missing file part
- `401` — Unauthorized
- `422` — Normalization failed (converter error)
- `503` — DB unavailable

### `POST /v1/docs/manifest`

Check which uploaded document hashes are absent

Request body (`application/json`).

Responses:

- `200` — Manifest diff
- `400` — Invalid manifest request
- `401` — Unauthorized
- `405` — Method not allowed
- `500` — Serialization failed
- `503` — DB unavailable

### `GET /v1/docs/{id}`

Retrieve doc metadata by id

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Responses:

- `200` — Document metadata
- `401` — Unauthorized
- `404` — Document not found

### `DELETE /v1/docs/{id}`

Delete a staged document

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Responses:

- `200` — Document deleted
- `401` — Unauthorized
- `404` — Document not found

### `POST /v1/drain`

Drain the asynchronous knowledge ingest queue

Request body (`application/json`).

Responses:

- `200` — Queue drained
- `401` — Unauthorized
- `405` — Method not allowed
- `500` — Queue drain failed

### `GET /v1/enrollments`

List issued client-certificate enrollments (console)

Paginated list of redeemed client certificates for the accounts surface.
Requires a console-admin credential.

Responses:

- `200` — Enrollment list
- `401` — Unauthorized
- `503` — Store unavailable

### `POST /v1/enrollments/{id}/revoke`

Revoke a client-certificate enrollment (console)

Marks the enrollment revoked; the revocation is enforced at the mTLS
seam. Requires a console-admin credential.

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Responses:

- `200` — Revoked enrollment
- `400` — Bad enrollment id
- `404` — Enrollment not found
- `503` — Revoke failed (store unavailable)

### `POST /v1/entities/search`

Find entities by name or context

Request body (`application/json`).

Responses:

- `200` — Entity search results
- `400` — Missing query
- `401` — Unauthorized

### `GET /v1/entities/{id}`

Canonical entity profile

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | string | Entity name (slug) |

Responses:

- `200` — Entity profile
- `401` — Unauthorized
- `404` — Entity not found

### `GET /v1/health`

Service health check

Returns {"status":"ok"} when the service is running.

Responses:

- `200` — Service is healthy

### `HEAD /v1/health`

Service health check (HEAD)

Responses:

- `200` — Service is healthy

### `POST /v1/ingest`

Enqueue background project ingest

Request body (`application/json`).

Responses:

- `202` — Ingest queued
- `401` — Unauthorized
- `405` — Method not allowed
- `503` — Knowledge store unavailable

### `GET /v1/ingest/status`

Report background project ingest status

Responses:

- `200` — Background ingest status
- `401` — Unauthorized
- `405` — Method not allowed
- `503` — Ingest status unavailable

### `POST /v1/intelligence/bandit/close`

Close a sampled decision with its observed reward

Responses:

- `200` — Close result
- `401` — Unauthorized

### `GET /v1/intelligence/bandit/export`

Export fusion bandit decision data

Responses:

- `200` — Bandit decisions and arm stats
- `401` — Unauthorized

### `POST /v1/intelligence/bandit/promote`

Persist the production-default arm for a decision point

Responses:

- `200` — Promotion result (rollback_arm)
- `401` — Unauthorized

### `POST /v1/intelligence/bandit/sample`

Sample an arm for a decision point (server-side decision points)

Responses:

- `200` — Selected arm + decision id (or status disabled)
- `401` — Unauthorized

### `GET /v1/intelligence/calibration/readiness`

Calibration readiness

Responses:

- `200` — Calibration readiness summary
- `401` — Unauthorized

### `GET /v1/intelligence/demotion/check`

Dry-run demotion readiness check

Responses:

- `200` — Demotion readiness summary
- `401` — Unauthorized

### `GET /v1/jobs/{job_id}`

Report asynchronous knowledge ingest job status

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `job_id` | path | yes | integer |  |

Responses:

- `200` — Job status
- `401` — Unauthorized
- `404` — Job not found
- `405` — Method not allowed
- `503` — Job status unavailable

### `POST /v1/maintenance/clear`

Clear indexed knowledge for a project

Request body (`application/json`).

Responses:

- `200` — Project cleared
- `400` — Missing project
- `401` — Unauthorized
- `500` — Clear failed

### `POST /v1/maintenance/reconcile`

Reconcile orphaned vector records

Request body (`application/json`).

Responses:

- `200` — Reconcile complete
- `401` — Unauthorized
- `500` — Reconcile failed

### `POST /v1/maintenance/repair`

Repair a project knowledge index

Request body (`application/json`).

Responses:

- `200` — Repair complete
- `400` — Missing required parameters
- `401` — Unauthorized
- `503` — Knowledge or vector store unavailable

### `GET /v1/pipeline/status`

Report asynchronous knowledge ingest queue status

Responses:

- `200` — Pipeline status
- `401` — Unauthorized
- `405` — Method not allowed
- `503` — Queue unavailable

### `POST /v1/releases`

Create a new corpus release

Request body (`application/json`).

Responses:

- `201` — Release created
- `400` — Missing name
- `401` — Unauthorized
- `409` — Name already exists

### `GET /v1/releases/active`

Get the currently active corpus release

Responses:

- `200` — Active release (or null if none)
- `401` — Unauthorized

### `POST /v1/releases/{id}/promote`

Promote a release to active

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Responses:

- `200` — Release promoted
- `400` — Invalid id
- `401` — Unauthorized
- `409` — Promote failed

### `POST /v1/releases/{id}/rollback`

Roll back to a prior release

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Request body (`application/json`).

Responses:

- `200` — Rolled back
- `400` — Invalid id
- `401` — Unauthorized
- `409` — No prior release to roll back to

### `GET /v1/review`

List staged documents pending review

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `cursor` | query | no | integer |  |
| `limit` | query | no | integer |  |

Responses:

- `200` — Review queue
- `401` — Unauthorized

### `POST /v1/review/{id}/accept`

Accept a staged document

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Request body (`application/json`).

Responses:

- `200` — Document accepted
- `400` — Invalid id
- `401` — Unauthorized
- `404` — Document not found

### `POST /v1/review/{id}/reject`

Reject a staged document

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Request body (`application/json`).

Responses:

- `200` — Document rejected
- `400` — Invalid id
- `401` — Unauthorized
- `404` — Document not found

### `GET /v1/scopes`

Scope lattice — distinct scopes with cert counts (console)

Responses:

- `200` — Scope list
- `401` — Unauthorized

### `POST /v1/search`

Hybrid knowledge search

Request body (`application/json`).

Responses:

- `200` — Search results
- `400` — Bad request (missing query)
- `401` — Unauthorized

### `GET /v1/version`

Service version

Responses:

- `200` — Version information
- `401` — Unauthorized

### `GET /v1/workers`

Report aimee-kb worker and background task status

Responses:

- `200` — Worker status
- `401` — Unauthorized
- `405` — Method not allowed
- `503` — Workers unavailable
