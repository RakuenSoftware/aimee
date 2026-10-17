# Aimee 1.0.2 deployment repair — 2026-10-17

The published 1.0.2 install on Proxmox .253 / CT100 needed operational qualification and two application repairs. The tested local image is an explicit hotfix on the published 1.0.2 base; it is not a newly published release.

## Repairs

Codex probes now use the canonical Vault token used by serving rather than an expired imported OAuth document. A read-only computed credential remains restricted to the canonical server connection. Named accounts cannot borrow it, explicit credential overrides retain precedence, and refresh errors fail closed. A file lock serializes the complete rotating-token refresh across serving threads and attested resource-helper processes. No refresh/access token was copied into configuration, environment, arguments, or a second credential store.

Hybrid code queries allow up to ten seconds for a CPU embedder already processing a document, capped at half the caller deadline to reserve time for SQL. Background index rebuilds reuse vectors only for identical SHA256 embedding inputs under the same verified serving identity and preprocessing policy. Each target retains its own source fingerprint and publication watermark checks. Unverified generations, other model identities, different paths, and changed inputs cannot use this cache.

The obsolete three-record native-preview fixture was retired at the operator's request. Publisher, follower, delivery, gateway and the gateway refresh watcher are disabled. Its configuration and records were preserved; its expired grant was not extended. The configured Codex and native personal-memory paths remain active.

The server has a stable container hostname. Explicit dashboard SANs define the stable deployment identity alongside localhost; automatic hostname/interface discovery is used only when no SANs are configured. This prevents changing Docker bridge addresses from needlessly replacing a certificate trusted for the LAN address. The Vault-held key is preserved, and declaring an additional stable SAN still renews the public certificate. The native API enrollment pin and instance identity were preserved. Optional graph expansion remains disabled because it exceeded the real repository query deadline during the initial deployment; lexical and semantic retrieval remain available.

## Validation

- Provider and memory Go suites pass. Canonical probes use the same current account slot as serving and do not borrow an account header from a stale OAuth import.
- Four runtime-web TLS tests pass: Vault-only key custody, necessary legacy renewal, deployment SAN parsing, and stable configured identities with explicit SAN extension without rekeying.
- C agent suite passes, including current-token selection and cross-process refresh serialization. Store-dependent tests were skipped in that isolated build; they are not claimed covered there.
- Disposable PostgreSQL tests pass for busy-embedder retrieval, short caller deadlines, stale content/model rejection, graph lookup, and reuse of identical verified inputs with changed-path/model/content negative cases.
- Live model probe: execution OK. A separate enrolled client's real Codex completion returns the exact expected assistant reply over its saved certificate pin and mTLS identity.
- Three enrolled-client hybrid queries return twenty results each, execute the dense path, and report Aimee's generation verified and ready. All 8,439 Aimee source files have current vectors and the active watermark matches publication.
- Restart persistence passes: the synthetic memory survived a server restart and was retired afterward; the probe, real completion and three dense retrieval checks pass again. Native API identity and the dashboard certificate remain unchanged. The dashboard serves its login page over the same trusted certificate.
- Other registered repository snapshots continue their background backfill. No incomplete generation was marked ready.

## Historical audit limit

The original ledger has 876 missing events, sequences 45210997–45211872, between 2026-10-03 12:33:46 and 12:39:25 UTC. The loss manifest and original/recovered databases are preserved. The available CT100 backup is dated 2026-09-21 and cannot supply those later events. Diagnostic bus captures do not contain the exact canonical WORM rows, wall-clock write timestamps and hash-chain commitments required to restore that history faithfully. Nothing in this repair fabricates entries, rewrites the historical chain, or converts its failure to green.

Final live verification confirms the active chain and checkpoint MACs are intact. The observation was amber with two new events after the checkpoint (head 14618687, checkpoint 14618685), not a new integrity break. A root-owned systemd timer now checkpoints the active head every sixty seconds, deferring while the server is not ready; it does not reset epochs or change historical verification.

Current audit-chain verification and checkpoint status are verified separately. Historical and aggregate verification must remain red unless an intact, independently verifiable copy of the missing history becomes available. The corrected SQLite runtime, capture-size limits, capacity checks and fail-closed admission from 1.0.2 remain in place.

Private deployment receipts, original configuration and test captures remain outside the repository under /root/aimee-deployment-repair-20261017 in CT100 and /var/tmp/aimee-v102-private locally. These directories must not be published. The pre-1.0.2 snapshot and original images are retained; snapshot restoration discards later writes and is not an ordinary image rollback.

The memory ownership ledger was reviewed for the Vault pipe change. The existing memory-backend credential custody symbol and Go ownership are unchanged; the new getter serves only the canonical Codex OAuth connection. The ledger digest and conformance references were updated without changing its owner, disposition, symbol inventory or original source reference.
