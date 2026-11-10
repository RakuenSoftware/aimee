# Aimee 1.1.0

This release opens the `1.1` series. `AIMEE_VERSION_SERIES` is `1.1`, and with no `v1.1.*` tag
yet the release process resolves the version as 1.1.0. It is a minor release, not a patch,
because the memory owner built into 1.0.x is replaced by a new, separate implementation.

## Memory is replaced

- The built-in Go memory owner, the Cognee and Hillock adapters and the memory evaluation tools
  are removed. Memory is now served by the aims-db database and its Aimee memory module. Their
  source is private; the application image carries them as a vendored binary bundle
  (`vendor/aims-memory`), verified against the tree before it is linked.
- Each deployment provisions its own installation. The one-shot `aimee-memory-init` Compose
  service generates the instance's memory identity, exact executable grants and owner key at first
  start, verifies them on later starts, and prepares the installation again after an image update
  while keeping the stored memory. The application mounts it read-only; records, configuration
  and the owner key live under `private-memory/` in the home volume.
- Aimee keeps the generic memory SDK, the module transport and the host-side guarantees of a
  provider-bound request: authenticated callers, context pre-injection, source release and
  revalidation, the byte budget, the send guard and provider receipts.
- The fixed `/v1/memory/*` routes are unchanged. `/v1/commands/memory.<verb>` reaches every command
  the implementation declares.

Limits of this release:

- **A KB does not accept shared memory writes.** The shipped implementation serves shared memory
  list and search on a KB, but answers every `memory.store` sent to the KB's API with
  `unsupported_mode`. New shared memories cannot be stored there and 1.0.x shared memories cannot
  be imported; they stay in the retained `memories` table. Personal memory on a Server is not
  affected. A KB that depends on shared memory writes should stay on 1.0.2.
- The memory bundle exists for `linux/amd64` only. An image for another platform carries no
  memory: its provisioning service fails with a message, and an instance without a provisioned
  installation reports `missing_module: memory` and is not ready.
- The Compose files need Docker Engine 26 or later and Compose 2.26 or later (volume subpath
  mounts). Start the stack through `scripts/compose-local.sh`, which reads the embedder image's
  identity for memory and runs provisioning.
- A Server serves personal memory for the accounts named in `AIMEE_MEMORY_ACCOUNTS` (default
  `aimee`, at most 32). An account created later is added by changing that list and restarting
  the stack.
- A change to any public file the bundle was compiled against needs a rebuilt bundle; lint and
  the image build refuse a stale one.

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
- Use this release's Compose files and embedder image, and start through
  `scripts/compose-local.sh`. Memory is provisioned on the first start and is ready but empty.
- Existing memories are retained in the 1.0.x tables (`user_memories` on a Server, `memories` on a
  KB) but are not read by the new implementation. `scripts/upgrade/migrate-memory.py` exports the
  active records read-only and, on a Server, imports them through the public store command,
  idempotently. Record ids, revision history and embeddings are not carried over. On a KB the
  export works and the import is refused; see the limits above.
- Records held by a Cognee or Hillock backend selected in 1.0.2 are not in those tables and are
  not moved by that tool.

Release publication follows the protected main-merge and release approval workflows. These notes
describe the prepared candidate; artifacts and deployment require those gates.
