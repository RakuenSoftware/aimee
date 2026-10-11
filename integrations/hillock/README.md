# Hillock retrieval proof of concept

Hillock is a selectable memory backend alongside native and Cognee. Selecting Cognee or Hillock starts an independent durable compatibility catalog under `$AIMEE_HOME/memory-backends/<engine>/<placement>` (override the root with `AIMEE_MEMORY_BACKEND_DIR`). The external memory owner does not construct a native PostgreSQL record store. Canonical content, scope, authorship, revisions, history and retry receipts persist in this catalog; the selected engine supplies retrieval. This is adapter-owned storage, not Hillock upstream's SQLite graph.

The sidecar uses the actual upstream `HyperdimensionalReservoir` subword encoder and HYDRA scorer at revision `1edd166ead75b85a9ab95cd6ba4faf7011ad567c`. It does not use upstream chat, TALON, graph or Hebbian learning. Each query supplies an eligible snapshot and receives IDs, revision hashes and scores, followed by canonical revalidation. The sidecar retains no records, vectors or conversation state.

The compatibility catalog uses locked atomic writes and retained erasure metadata. Corrections preserve prior versions; deletes retire records; subject erasure removes content/history/retry payloads and prevents restoration when its retained `erasures.json` is preserved. Back up the complete catalog directory and preserve current erasure control metadata when restoring older record files. Startup replays these controls and verifies provider cleanup before readiness.

The [external conversation contract](../memory/CONTRACT.md) is implemented through Aimee's adapter: public CRUD/list/history, recall/composition/briefing, authenticated export, source revalidation/send barriers, and fresh admission after erasure. Portable catalog snapshots preserve IDs, revisions, history, authorship and erasure controls; the maintenance tool bridges native record snapshots. Optional native graph/learning/views remain outside this profile. [Current validation](../../docs/validation/external-memory-support-2026-10-08.md) and [pending quality/scale work](../../docs/proposals/pending/external-memory-engine-scale-and-quality.md) state the practical limits.

## Run

From this directory, supply a mode-0600 token file owned by container UID 10001 (a narrowly readable container secret mount can also be used), then:

```sh
HILLOCK_TOKEN_FILE=/absolute/path/to/token docker compose up -d --build
```

The compose example exposes only loopback port 8097, runs as UID 10001 with a read-only filesystem, and sets resource limits and an authenticated health check. For another container, attach it to the owner's private service network and use its service address; do not use the owner's loopback address to reach a different container.

Configure the existing Aimee memory owner:

```sh
AIMEE_MEMORY_BACKEND=hillock
AIMEE_MEMORY_BACKEND_URL=http://127.0.0.1:8097
AIMEE_MEMORY_BACKEND_AUTH=bearer
```

Put the matching bearer token in the existing Vault key `AIMEE_MEMORY_BACKEND_TOKEN`; do not put it in these configuration values. `AIMEE_HILLOCK_URL` is a fallback for the URL. `AIMEE_MEMORY_BACKEND_AUTH=none` and sidecar `HILLOCK_AUTH=none` are explicit alternatives for an isolated test network. Startup verifies the exact profile/revision and stateless contract before readiness. Unknown backends, unsupported profiles, outages and capacity failures do not silently select native retrieval. Native remains the default when the backend setting is empty.

## Implemented API and limits

- `GET /v1/health`: protocol version, upstream revision, `hdc-subword` profile and `stateless=true`.
- `POST /v1/rank`: JSON `{query, candidates:[{id, revision, text}], limit}` → `{hits:[{id, revision, score}]}`. Higher scores rank first, equal scores use ascending record ID. The fixed 0.20 match threshold is a POC retrieval setting, not confidence that a claim is true.
- Maximum 256 candidate records per rank request, 1 MiB request/response, 16 KiB query, 64 query tokens, 256 tokens per record and 4096 distinct snapshot/query tokens, with at most 128 UTF-8 bytes per token. Exceeding a bound returns a capacity error (HTTP 413); Aimee scans the eligible catalog and selects a bounded lexical candidate pool; capabilities advertise non-exhaustive retrieval. This is not a 256-record catalog limit.
- CPU work is limited to 10 seconds per request; a compute deadline returns HTTP 504. The service processes one ranking request at a time with a bounded socket backlog.
- Errors: 400 malformed input, 401 missing/wrong bearer, 404 unsupported route, 413 capacity. Aimee treats other failures as unavailable. Empty canonical searches list directly from the selected catalog without invoking Hillock.

Subword similarity is lexical/morphological retrieval, not a claim of semantic equivalence, independent corroboration or graph reasoning. Aimee must still apply its evidence/authority and answer composition rules.

## Verify against real upstream code

```sh
python3 -m venv /tmp/hillock-poc-venv
/tmp/hillock-poc-venv/bin/pip install -r integrations/hillock/requirements.txt
/tmp/hillock-poc-venv/bin/python scripts/validation/memory/run-hillock-contract.py \
  --artifacts /tmp/hillock-poc-evidence-new-run
```

Run those verification commands from the repository root. The runner checks out the pinned upstream, rejects modified tracked source, starts an authenticated service on an ephemeral loopback port, requires the live Go test to execute, verifies corrections/deletion/capacity and no cross-request retained retrieval, then repeats after a process restart. It retains fixture-only logs and a summary. Unit/race tests also reject malformed identities, changed revisions, outages and cancellation. CI runs the real contract with pinned NumPy. These checks establish POC behavior, not production-scale quality or measured conversational improvement. The Docker deployment recipe is supplied; the local evidence does not claim a Docker build or live Discord rollout.

[Current .253 support validation](../../docs/validation/external-memory-support-2026-10-08.md) records the completed integration checks and remaining quality limits.
