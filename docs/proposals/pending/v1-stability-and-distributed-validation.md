# Proposal: Close out platform phase 7 — v1 API stability tag + distributed-mode validation

- **State:** proposed (pending — not started)

## Thesis

The aimee-kb platform arc (roadmap "aimee-kb service + public API") landed phases
1–6: the service split, OpenAPI + SDKs, ingest, corpus staging, the retrieval
surface, and reflection HTTP all exist and are conformance-checked. **Phase 7 —
"distributed-mode validation + v1 API stability tag" — is the one piece with no
evidence it happened.** The `/v1` surface is exercised piecemeal by a dozen
conformance/coverage checks, and distributed mode *works in the field* (aimee-kb
runs remotely with thin clients today), but there is no single test that stands up
the split topology end-to-end, and no declared, enforced "v1 is stable — breaking
it is a versioned event" contract. Until that exists, v1 is *de facto* stable and
*de jure* undefined.

## Goal

1. A **v1 stability contract**: a machine-checkable declaration of the frozen v1
   surface, with a CI gate that fails a PR that changes it without an explicit
   version bump.
2. A **distributed-mode integration test**: one harness that runs aimee-kb as a
   remote service with ≥2 concurrent thin clients against it, exercising the real
   `/v1` paths (ingest → curate → retrieve, auth, per-operator scoping) — the
   topology we actually ship, tested as a unit.

## §0 What already exists

- **Broad but piecemeal conformance.** `api-conformance-check`,
  `server-api-conformance-check`, `v1-method-coverage-check`,
  `cli-v1-routes-check`, `kb-v1-coverage-check`, `sdk-parity-check`,
  `v1-third-party-test`, `v1-sdk-smoke`, `v1-ws-test` — plus OpenAPI at
  `docs/gen/api-v1.md` and `gen-sdks`.
- **Distributed mode runs in production.** aimee-kb as a remote plugin, thin
  clients over `/v1` (the `.254` deployment) — but validated only by usage, not
  a committed test.
- **No frozen-surface gate + no topology test.** Nothing declares "this is v1,
  frozen" or stands up server+clients together in CI.

Phase 7 is the *closeout*, not new construction.

## §1 v1 surface snapshot + stability gate

Generate a canonical, sorted snapshot of the v1 contract (routes + methods +
request/response schema, derived from the same source the conformance checks read)
into `docs/gen/v1-surface.lock`. Add a `v1-stability-check` make target + CI job
that regenerates and diffs it: any change fails unless the PR also bumps the
declared `API_VERSION` / adds a `v1.<n>` compatibility note. Additive-only changes
(new optional field, new route) pass with a changelog line; removals/renames
require the version event. This turns "don't break v1" from a norm into a gate.

## §2 Distributed-mode integration harness

Add `make v1-distributed-test`: bring up aimee-kb (its own DB2) as a detached
service, point ≥2 thin clients at it, and run a scripted end-to-end —
enroll/auth, `POST /v1/docs` + code scan from client A, curate, `POST /v1/search`
+ `/v1/code/*` from client B, and assert per-operator scoping isolates A's and B's
writes. Run it in CI (the same runner that already builds the split images).

## §3 Concurrency + isolation assertions

The harness is also where the boundary claims get teeth: two clients writing the
same project concurrently must not corrupt provenance; a client with scope X must
not read scope Y; the KB pool must not wedge under concurrent ingest (see the
curator-drain lease work). Encode these as explicit assertions, not incidental.

## §4 Tag + document

Once §1–§3 are green, tag the v1 surface stable in `docs/gen/api-v1.md` and record
the versioning policy (additive vs. breaking, deprecation window) next to it, so
downstream SDK/thin-client authors have a contract, not folklore.

## Non-goals

Not redesigning auth (that is its own deferred multi-tenant item) and not adding
API surface — this freezes and validates what phases 1–6 already shipped.
