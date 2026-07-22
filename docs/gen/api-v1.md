# aimee-kb API — v1

> Auto-generated from `api/openapi-v1.yaml` by `scripts/gen-api-docs.py`. Do not edit by hand; run `make docs-gen` to regenerate.

Total endpoints: 80

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

### `GET /v1/audit/actions`

Policy-verdict action audit feed (console)

Requires a `since` time-window bound; optional `until`, `scope_kind`, `limit`.

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `since` | query | yes | string |  |

Responses:

- `200` — Audit action list
- `400` — Missing required time-window
- `401` — Unauthorized

### `POST /v1/budget/set`

Set a team/project period budget cap (org-admin, P4a)

Upserts the hard-cap config for (team, optional project, period). Org-admin gated at the DB layer and WORM-audited atomically with the mutation; a non-admin caller receives 403. A hard reduction of the limit below the current period's already committed (spend + reserved) is rejected as retroactive (409). limit_usd and soft_limit_usd are NUMERIC decimal strings (never floats). soft_limit_usd is a config-only operator-signal threshold (P4a does not enforce it — a soft limit never refuses). BUDGET ONLY: the rate limiter is deferred to P4b; the reserve-before-dispatch enforcement rides with P2b.

Request body (`application/json`).

Responses:

- `200` — Budget cap upserted
- `400` — Missing or malformed team/period/limit_usd/project/soft_limit_usd
- `401` — Authentication required
- `403` — Not authorized (not an org-admin)
- `409` — Retroactive reduction below current committed spend+reserved

### `GET /v1/budget/show`

Show a team's budget caps + current-period counters (org-admin or team-lead, P4a)

Returns every configured cap for the team (optionally filtered to one project), each joined to its current UTC period counter: limit, spend, reserved, and remaining (= limit - spend - reserved). Authorization is enforced at the DB layer inside a SECURITY DEFINER function — the caller must be an org-admin OR a lead of the requested team. All money fields are NUMERIC strings (never floats).

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `team` | query | yes | integer | Team id (required). |
| `project` | query | no | integer | Optional project filter; absent shows every cap for the team. |

Responses:

- `200` — Budget caps + current-period counters
- `400` — Missing or malformed team/project
- `401` — Authentication required
- `403` — Not authorized (org-admin or team-lead required)

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

### `GET /v1/config/oidc`

Console OIDC login config (console-admin)

Responses:

- `200` — OIDC config (empty if unset)
- `401` — Unauthorized

### `PUT /v1/config/oidc`

Set the console OIDC login config (console-admin)

Structural validation (https jwks_url + required fields). The console
fetches this at startup; restart the console to re-apply.

Responses:

- `200` — Stored config
- `400` — Bad request
- `503` — Config store unavailable

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

### `GET /v1/decisions`

List governance decision records (console)

Filter by subject/status; most-recent-first. Console-admin only.

Responses:

- `200` — Decision list
- `401` — Unauthorized

### `POST /v1/decisions`

Author a decision record (console)

Creates a decision; one active decision per (subject, linked_policy_id).
A conflicting create returns 409 (supersede the active one instead).

Responses:

- `201` — Created decision
- `400` — Bad request
- `409` — Conflict — an active decision already exists for this scope

### `GET /v1/decisions/{id}`

Decision record + supersede chain (console)

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `id` | path | yes | integer |  |

Responses:

- `200` — Decision with supersede_chain
- `404` — Not found

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

### `GET /v1/insights/spend`

Authorized org spend report, grouped per model + per project (P3b)

Returns org spend aggregated over the settled rollup for day in [since, until], broken down per billable model and per project with a reconciling total (the sum of the per-model costs equals total.cost_usd exactly). Authorization is enforced at the DB layer inside a SECURITY DEFINER aggregation function: the caller must be an org-admin OR a lead of the requested team; a team-absent request is the org-wide report and is admin-only. cost_usd is emitted as a NUMERIC string (never a float) so finance export loses no precision. Read-only.

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `team` | query | no | integer | Team id. Absent = the org-wide (admin-only) report. |
| `project` | query | no | integer | Optional project filter. |
| `since` | query | yes | string | Inclusive start day, ISO YYYY-MM-DD. |
| `until` | query | yes | string | Inclusive end day, ISO YYYY-MM-DD. |

Responses:

- `200` — Spend report
- `400` — Missing or malformed team/project/since/until
- `401` — Authentication required
- `403` — Not authorized (org-admin or team-lead required)

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

### `POST /v1/maintenance/purge-cancel`

Clear the project-purge fence on abort

Request body (`application/json`).

Responses:

- `200` — Clear outcome (cleared false on fence mismatch)
- `400` — Missing or invalid parameters
- `401` — Unauthorized
- `500` — Clear failed

### `POST /v1/maintenance/purge-finalize`

Clear the project-purge fence after deletion completed

Request body (`application/json`).

Responses:

- `200` — Clear outcome (cleared false on fence mismatch)
- `400` — Missing or invalid parameters
- `401` — Unauthorized
- `500` — Clear failed

### `POST /v1/maintenance/purge-heartbeat`

Refresh the project-purge fence heartbeat

Request body (`application/json`).

Responses:

- `200` — Heartbeat outcome (refreshed false on fence mismatch)
- `400` — Missing or invalid parameters
- `401` — Unauthorized
- `500` — Heartbeat failed

### `POST /v1/maintenance/purge-project`

Purge every kb store for a project under a generation fence

Writes the project-purge generation fence, then deletes the project from every kb store the ingest path writes (chunks, file index, vectors, code embeddings, curator code-unit vectors, canonical index, code-unit jobs, pdf vectors, minhash), continuing past per-store failures. The fence is NOT cleared here — call purge-finalize (or purge-cancel) once the caller's own deletion completed. Idempotent.

Request body (`application/json`).

Responses:

- `200` — Purge fan-out completed (see per-store outcomes)
- `400` — Missing or invalid parameters
- `401` — Unauthorized
- `409` — A live fence is held by another owner (takeover absent)
- `500` — Fence write failed

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

### `GET /v1/models/entitled`

List the caller's entitled org models (P2a)

Returns the org models the authenticated caller is entitled to use — the join of the org model catalog with the caller's team entitlements, actor-bound to the verified principal (a caller can never see another principal's entitled models). Catalog-only: the surface carries provider/wire/endpoint/model_id/display_name and NO credential or slot reference. Disabled catalog entries are excluded.

Responses:

- `200` — Entitled models
- `401` — Authentication required

### `POST /v1/models/org/add`

Create or update an org model catalog entry (org-admin, P2a)

Upserts a catalog entry (keyed by model_id). Org-admin gated at the DB layer and WORM-audited atomically with the mutation; a non-admin caller receives 403.

Request body (`application/json`).

Responses:

- `200` — Catalog entry upserted
- `400` — Invalid model_id, provider, or wire
- `401` — Authentication required
- `403` — Not authorized (not an org-admin)

### `POST /v1/models/org/entitle`

Grant a team access to an org model (org-admin, P2a)

Request body (`application/json`).

Responses:

- `200` — Entitlement granted
- `401` — Authentication required
- `403` — Not authorized (not an org-admin), or unknown model/team

### `POST /v1/models/org/remove`

Remove an org model catalog entry and its entitlements (org-admin, P2a)

Request body (`application/json`).

Responses:

- `200` — Catalog entry removed
- `401` — Authentication required
- `403` — Not authorized (not an org-admin)

### `POST /v1/models/org/set`

Alias of /models/org/add — upsert an org model catalog entry (org-admin, P2a)

Request body (`application/json`).

Responses:

- `200` — Catalog entry upserted
- `400` — Invalid model_id, provider, or wire
- `401` — Authentication required
- `403` — Not authorized (not an org-admin)

### `POST /v1/models/org/unentitle`

Revoke a team's access to an org model (org-admin, P2a)

Request body (`application/json`).

Responses:

- `200` — Entitlement revoked
- `401` — Authentication required
- `403` — Not authorized (not an org-admin)

### `GET /v1/pipeline/status`

Report asynchronous knowledge ingest queue status

Responses:

- `200` — Pipeline status
- `401` — Unauthorized
- `405` — Method not allowed
- `503` — Queue unavailable

### `GET /v1/project`

List projects (optionally filtered by ?team=)

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `team` | query | no | integer |  |

Responses:

- `200` — Projects
- `401` — Authentication required

### `POST /v1/project`

Create a project under a team (org-admin)

Request body (`application/json`).

Responses:

- `201` — Project created
- `401` — Authentication required
- `403` — Not authorized

### `POST /v1/rate/policy`

Set a keyed fixed-window rate-limit policy (org-admin, P4b)

Upserts the admin-set rate policy for (dim, scope). dim is the limiter dimension (team | project | cert | model | cred_slot); scope is the concrete id/name, or "*" for the dim default applied when no specific row exists. window_seconds is the fixed-window width and max_count the requests admitted per window (max_count = 0 is an always-deny). Org-admin gated at the DB layer and WORM-audited atomically with the mutation; a non-admin caller receives 403. The policy is authoritative and never caller-supplied at enforcement time — the P2b egress path passes only the resolved identity to org_rate_check, which looks the policy up. RATE ONLY: the enforcement wiring at egress rides with P2b; the budget core is P4a.

Request body (`application/json`).

Responses:

- `200` — Rate policy upserted
- `400` — Missing or malformed dim/scope/window_seconds/max_count
- `401` — Authentication required
- `403` — Not authorized (not an org-admin)

### `GET /v1/rate/show`

Show a keyed rate-limit policy (org-admin or team-lead, P4b)

Returns the rate policy for the exact (dim, scope) pair (0 or 1 row). Authorization is enforced at the DB layer inside a SECURITY DEFINER function — the caller must be an org-admin, OR (for dim=team) a lead of that team, OR (for dim=project) a lead of the team that owns the project. The global dims (model, cred_slot) and the "*" default are admin-only. Read-only.

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `dim` | query | yes | string (team, project, cert, model, cred_slot) | Limiter dimension (team | project | cert | model | cred_slot). |
| `scope` | query | yes | string | The concrete id/name, or "*" for the dim default. |

Responses:

- `200` — Rate policy (0 or 1 row)
- `400` — Missing or malformed dim/scope
- `401` — Authentication required
- `403` — Not authorized (org-admin or team-lead required)

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

### `GET /v1/servers/{server_id}/health`

Verify a registered server through the management health exchange

Performs the live, nonce-bound management challenge and signed-status exchange. It does not return the registry's cached heartbeat row.

| Name | In | Required | Type | Description |
|------|----|----------|------|-------------|
| `server_id` | path | yes | string |  |
| `team` | query | yes | integer |  |

Responses:

- `200` — Live management health verification succeeded
- `400` — Invalid team or management health request
- `401` — Authentication required
- `403` — Server health request denied
- `404` — Server not found
- `409` — Registry state changed during the exchange
- `502` — Management health integrity verification failed
- `503` — Management health runtime or dependency unavailable

### `GET /v1/team`

List the caller's visible teams

Responses:

- `200` — Teams
- `401` — Authentication required

### `POST /v1/team`

Create a team (org-admin or bootstrap owner)

Creates an org team. The org-admin capability is enforced at the DB layer (RLS write policies); a non-admin caller receives 403.

Request body (`application/json`).

Responses:

- `201` — Team created
- `401` — Authentication required
- `403` — Not authorized (not an org-admin)

### `POST /v1/team/member`

Add a member to a team (org-admin)

Request body (`application/json`).

Responses:

- `200` — Member added
- `401` — Authentication required
- `403` — Not authorized

### `DELETE /v1/team/member`

Remove a member from a team (org-admin)

Request body (`application/json`).

Responses:

- `200` — Member removed
- `401` — Authentication required
- `403` — Not authorized

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
