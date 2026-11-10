# Aimee 1.1.0

This release opens the `1.1` series. `AIMEE_VERSION_SERIES` is `1.1`, and with no `v1.1.*` tag
yet the release process resolves the version as 1.1.0. It is a minor release, not a patch,
because memory is no longer served by the owner built into 1.0.x.

## Memory leaves the application image

- The built-in Go memory owner, the Cognee and Hillock adapters and the memory evaluation tools
  are no longer part of the public tree or image. Aimee keeps the generic memory SDK, the module
  transport and the host-side guarantees of a provider-bound request: authenticated callers,
  context pre-injection, source release and revalidation, the byte budget, the send guard and
  provider receipts.
- The bundled `memory` executable is an explicit unavailable adapter. An instance without an
  installed memory implementation starts, serves chat and its other routes, reports
  `missing_module: memory` in `/v1/ready`, stays unhealthy and answers memory commands with `503`.
- A memory implementation is installed separately and selected in the role module manifest. It
  declares the `memory.*` commands it serves; the fixed `/v1/memory/*` routes are unchanged and
  `/v1/commands/memory.<verb>` reaches every declared command.
- Public CI tests the adapter, the SDK and the host paths. It does not certify an installed
  implementation, whose own qualification accompanies its deployment.

## Runtime

- Every module process is Go. The C module processes were retired and unreachable C was removed;
  the C resource hosts, the CLI and the bus embedding remain.
- The ouroboros ring is consumed by contract from its own repository
  (`dependencies/ouroboros-ring.json`).
- An idle instance does no polling; ring events leave the host as they are stamped.
- Browser chat, delegates and history use one Server-owned canonical IR session log.
- A per-session cache with compare-and-set receipts is available to host components.
- Memory-owner calls made during ingress pre-injection are bounded by the remaining receipt
  deadline, at most 500 ms each.

## Fixes

- An oversized provider request is refused as `request_budget_exceeded` (HTTP 413) even when the
  memory implementation had already refused it, so every implementation classifies the same input
  the same way. Nothing is sent in either case.
- `/v1/chat/completions` with `stream: true` ended with two `data: [DONE]` events; it now ends
  with one.

## Upgrade notes

Read [Upgrading from 1.0.2](../../UPGRADING.md#upgrade-from-102) first. In short:

- Back up the home, Vault, PostgreSQL, workspaces and audit evidence as a matched set. The new
  version applies schema migrations; going back means restoring that backup onto 1.0.2 images.
- Install a memory implementation as part of the upgrade. Without one the upgraded instance is
  not ready and serves no memory.
- Existing memories are retained in the 1.0.x tables (`user_memories` on a Server, `memories` on a
  KB) but are not read by an installed implementation. `scripts/upgrade/migrate-memory.py` exports
  the active records read-only and imports them through the implementation's public store
  command, idempotently. Record ids, revision history and embeddings are not carried over.
- Records held by a Cognee or Hillock backend selected in 1.0.2 are not in those tables and are
  not moved by that tool.
- An implementation that pins the embedding identity needs an embedder whose `/health` reports
  `embedding_identity`.

Release publication follows the protected main-merge and release approval workflows. These notes
describe the prepared candidate; artifacts and deployment require those gates.
