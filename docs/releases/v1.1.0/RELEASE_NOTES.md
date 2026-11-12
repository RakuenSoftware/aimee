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

- Shared facts on a KB are stored as written; no extraction pipeline runs on them, so typed
  facts are not derived from shared memory even with a synthesis model configured.
  `memory.facts` returns an empty block, as a 1.0.x install without a synthesis model did, and
  `memory.episode_card_generate` answers `disabled`. Extraction on a KB is planned for the next
  release. A KB serves global and every project and workspace by default;
  `AIMEE_MEMORY_SHARED_SCOPES` can restrict it to an exact list.
- A Server enrolled into a separately deployed KB serves personal memory only, as in 1.0.x: the
  KB link carries no memory, so a shared-store request from that Server answers a typed,
  retryable `unavailable`, and with `kb_mode` `remote` and automatic recall on a run refuses its
  memory context. Use shared memory on the KB itself, or set `kb_mode` to `none` on that Server.
- Memory refuses a record whose text contains a raw sensitive value (an email address, phone
  number, IP address, password or key) and says the value belongs in the vault. 1.0.x stored
  such records and withheld them from injected context.
- Memory maintenance is the installation owner's operation. When aimee prunes ended sessions it
  folds each one's raw (L0) memories into a summary through `memory.compact_windows`, under a
  per-owner maintenance operator that only aimee's own in-process call can use; the folded
  memories are archived, not deleted. 1.0.x folded sessions older than 30 days; the memory
  service keeps no record age, so it folds the sessions aimee names as ended instead.
  `memory.maintenance_run` reports replay and folds named sessions; `prune` (which deleted
  memories in 1.0.x), `summarize` and `drift` are refused with the reason. A model, a public
  caller or another account cannot run either command. `memory.embed`, `repair`, `rebuild`,
  `reindex` and `reembed_*` answer `not_available` because the memory service maintains its own
  vector index; an embedding model change is applied by provisioning when the embedder image
  changes.
- The guardrail's task-drift check is answered by the KB's task store (`task.check_drift`).
- Server and KB images are published for `linux/amd64` only; `linux/arm64` server images are no
  longer built. The memory bundle exists for that platform and the image build refuses any
  other. Thin clients are still published for every platform.
- The Compose files need Docker Engine 26 or later and Compose 2.26 or later (volume subpath
  mounts). Start the stack through `scripts/compose-local.sh`, which reads the embedder image's
  identity for memory and runs provisioning.
- A Server serves personal memory for the runtime account, every browser login and any account
  named in `AIMEE_MEMORY_ACCOUNTS`, at most 32 in all. The list is re-evaluated at each start, so a
  browser login created while the stack runs gets personal memory at the next restart.
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
  active records read-only and imports them through the public store command, idempotently. On
  a KB, `migrate-memory.py scopes` prints the shared scopes to declare first. Record ids,
  revision history and embeddings are not carried over.
- Records held by a Cognee or Hillock backend selected in 1.0.2 are not in those tables and are
  not moved by that tool.

Release publication follows the protected main-merge and release approval workflows. These notes
describe the prepared candidate; artifacts and deployment require those gates.
