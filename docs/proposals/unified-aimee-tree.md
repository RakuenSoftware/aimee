# Unified aimee tree — implementation contract

Status: implementation in progress. The decisions below are settled.

## Topology and ownership

- Every installation is **aimee**, with a local store, embedder, API, users and directly connected children. Retire the `aimee-server` / `aimee-kb` role split, `AIMEE_INSTANCE_ROLE`, and role-specific compositions after migration.
- Each node has zero or one configured parent and zero or more direct children. Links are authenticated with existing enrollment, mTLS and P5 management transport.
- On attachment, reject self-parenting and cycles by verifying the proposed child's ID is absent from the proposed parent's entire ancestor chain. Reject if ancestor verification cannot finish; serialize concurrent reparenting under a topology revision/fence so two simultaneous requests cannot create a cycle.
- Children never address siblings through their parent as an RPC router. Information replication is a separate, parent-mediated distribution operation.

## Shared identity

Use a single trusted OIDC issuer across a tree. Canonical principal identity is `oidc:<iss>:<sub>`, derived from validated token claims, not caller-supplied strings. Preserve caller context and enforce resource authorization at every node; possessing a common principal ID must not implicitly grant access to another node's private data.

## Configuration authority

- Managed values inherited from ancestors are not locally overridable. If multiple ancestors enforce the same key, **the highest ancestor wins** (closest to the root), not the closest parent.
- Propagate the originating node ID, enforcement flag and version/revision with each managed key; intermediate nodes must preserve the origin and cannot replace higher-origin enforced values.
- Reject local writes to inherited managed keys and expose the originating aimee in effective-config reads.
- Reject unauthorized or stale configuration updates and preserve previously verified enforced values across disconnects. Root/ancestor changes cannot be invented during a partition.

## Disconnected operation

A node with an unreachable parent continues to operate locally in single-user mode. No remote parent connection is required for its local store, embedder or ordinary local operations. It must not bypass previously enforced configuration just because the parent is unreachable.

- Queue eligible shared-record changes durably for replay after reconnection; maintain idempotency and convergence. No request should silently drop local data when offline.
- Existing locally cached allowed shared data remains usable, but **new remote sharing and parent-only management** remain unavailable until connected.
- Disconnect does not silently detach the node or grant elevated administrator authority.

## Information dissemination

- Child -> parent sends only *shareable records*; parent stores and distributes allowed records to its other descendants through each direct child channel. Local records and shared replicas are distinguishable.
- Transfer canonical records and metadata, **never embeddings/vectors**. Receiving node embeds with its own configured width.
- Design a stable origin+record ID, revision, change kind (including tombstones), causal/version metadata and idempotent replication. Deduplicate echoes, prevent endless bounce, honor authorization and deletions, and synchronize after outages.
- Information shared by one child is discoverable by other authorized descendants under the same parent. Data under an ancestor reaches all authorized descendants of that ancestor.

## Local-only boundary

- PII, passwords, credentials, secrets, vault material, and machine/environment settings are **always local** and may not traverse any link in either direction.
- Initially extend the existing `kb_client_pii_*` / `memory.screen_content` detection rather than replacing it. Apply screening at the common outbound link serialization boundary **in both directions**; also validate inbound payloads defensively.
- Fail closed on unknown/failed classification: hold data locally, do not transmit. Inspect nested fields, metadata, identifiers, change events and batches, not only plain-text content. Redaction must not disclose meaningfully reconstructable secrets.
- Local-only records are stored in their originating node's store, not discarded as in the split-server architecture. Transport protocol credentials used for link authentication are distinct from replicated application data.

## Implementation areas

1. Unify binary and deployment; migrate existing `compose.kb.yaml` / `compose.server*.yaml` installations without losing local data.
2. Ensure every node initializes its own store and embedding runtime; retire forwarding-only `kb_client_memory_*` semantics.
3. Support variable embedding widths end-to-end; evidence embedding previously assumed 384.
4. Reuse `kb_client_mtls.c`, `enroll.c`, `pki.c`, P5 registry/control channel for one parent and direct child management.
5. Implement topology registry, ancestor proof, concurrent cycle defense and reconnection.
6. Reuse `auth_oidc.c` and `X-Aimee-Caller-Authorization` with validated principal propagation.
7. Add inherited effective config and managed write protection around `server_http_config_routes.c` / P5 admin.
8. Build record replication, local-only classification, durable offline replay, tombstones, and re-embedding.

## Required verification

- 1/2/3+-level trees, self-parent and ancestor cycle rejection, concurrent attach races, reparenting and restart persistence.
- Top ancestor vs immediate parent config conflict, attempts to override managed keys, offline restart retaining enforcement.
- Parent link outage: local usage succeeds, durable records replay exactly once on reconnect.
- Sibling dissemination without sibling RPC routing, cross-tree isolation, authorization checks, dedupe and tombstone propagation.
- PII/secrets/environment nested in payloads never appear on either link direction; classification unavailable => no transfer and local data intact.
- Different embedder widths across nodes (including 384 and 768/1024), storage/query/evidence compatibility, no vectors crossing links.

This contract does not assert that the full tree implementation is finished.
