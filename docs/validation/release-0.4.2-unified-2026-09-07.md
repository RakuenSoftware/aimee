# Unified deployment candidate for 0.4.2

The requested deployment architecture is implemented in this candidate. Release
qualification remains open until the updated PR checks, image publication and
release provenance gates pass. This is evidence from disposable deployments on
a dedicated Linux VM hosted by `.253`; it is not a claim that production or the
published `:testing` image has already been upgraded.

## Implemented behavior

- One application image supplies immutable Server or KB identity, established at
  first boot. Both identities are Go composition modules; core event-bus admission
  rejects conflicting and duplicate roles. The shared supervisor manages required
  standard modules and restarts failed children. Existing native resource and
  protocol hosts remain part of the application.
- Both roles use the same PostgreSQL and memory modules. Server personal records
  and derived vectors stay in its own store. KB supplies shared knowledge and has
  its own Vault, PostgreSQL and models. KB is absent from base Compose and wizard
  installation; connecting to an existing KB remains available in Settings.
- Base deployment includes local embedding. Synthesis is optional; local model
  services authenticate with identities belonging to their own composition.
  Compatible external model targets remain available.
- PostgreSQL 18 with vector extensions uses the standard LUKS2 storage container.
  Its unlocking credential persists only in the instance Vault. The credential
  moves through locked transient memory and private pipes, with no TPM, external
  key store, keyfile, environment or command-line key transport. Storage fails
  closed when Vault, identity or ciphertext validation fails.
- Database and enrollment credentials enter the application Vault through a
  disposable stdin bootstrap. Application container metadata contains neither
  SQL DSNs nor enrollment credentials. PostgreSQL initialization role passwords
  remain distinct from the Vault-only LUKS unlocking credential.

## Validation

| Check | Observed result |
| --- | --- |
| Full native unit suite | Passed; negative-path diagnostic logs are expected |
| All Go packages with race detector | Passed |
| Live PostgreSQL workflow store, API and engine | Passed with race detector and required real store fixtures |
| Complete script regression suite | All 51 existing suites plus the new export-pin preservation regression passed; all discovered suites now run in CI |
| Frontend tests and production build | 191 tests passed; build passed |
| Lint and build integrity | All 77 lint checks and build integrity passed |
| Shipped Compose variants | Eight normalized compositions passed |
| Standalone exports | Core, DB2, Server, KB, PostgreSQL, memory and providers built independently |
| Git history and new commit secret scans | 5,346 historical commits plus the implementation commit scanned; no leaks |
| Real PostgreSQL LUKS fresh volume | 11 checks passed |
| PostgreSQL 18 offline plaintext migration | 13 checks passed, record preserved through restart |
| Fresh browser setup | Login, API-keyless local primary, model selection/deployment, completion and optional KB Settings passed |
| Real native synthesis request | C client completed inference through the managed mTLS model service |
| Connected deployment | Personal/shared scope isolation, enrollment, restart and outage tests passed |
| KB-free deployment | Personal memory and real local semantic recall, expiry, mutation and outage recovery passed |

The final application image is `aimee:unified042-v28`, digest
`sha256:9a8b4e5135127fd5b0aac66d87bad0479692d0e3bc81be4587003eb440868e58`,
built from commit `122cc89bf8bca55e9905c744a9d51bb7fe937a41`. It reports
`aimee v0.4.2`.
[T1](release-0.4.2-unified-2026-09-07/t1-topology.json),
[T2](release-0.4.2-unified-2026-09-07/t2-topology.json), and
[T3](release-0.4.2-unified-2026-09-07/t3-topology.json) passed. Their component
gates contain 70 connected-memory, 15 local-memory per Server topology,
15 semantic-memory and six immutable-identity checks. Fresh browser setup passed
on v11. The same managed browser deployment then upgraded to v28 and passed real
login, model retirement and reinstallation. The native HTTP regression separately
verifies that selecting No KB disables an old managed identity, including cached
connections and restart, and that selecting remote reconnects. Exact image
[digests](release-0.4.2-unified-2026-09-07/images.json) are recorded separately.

The full [analyzer run](release-0.4.2-unified-2026-09-07/static-analysis.json)
processed 981 sources with 3,352 advisory warnings and zero process failures.
The later mTLS change was analyzed with seven advisory warnings and zero process
failures. Cppcheck 2.13.0 checked all 32 changed production C files with four known
diagnostics and no increase over the reviewed baseline. These are recorded in
[follow-up analysis](release-0.4.2-unified-2026-09-07/static-analysis-followup.json).
This is not a warning-free claim. Earlier results are preserved in the
[storage report](release-0.4.2-postgres-luks-2026-09-06.md) and
[local recall report](release-0.4.2-local-recall-2026-09-06.md).

Reproduce deployment gates with `tests/e2e/deployment-matrix.py --topology T1`,
`T2`, or `T3`, supplying the candidate application, PostgreSQL and embedder images
and an output directory. Each run creates independent random projects and removes
its own resources. The three historical Docker smoke commands invoke this same
harness. `scripts/validation/setup/browser.cjs` exercises the real browser wizard;
its credentials file and screenshots must remain private.

## Scope and operational limits

- This changes composition and local memory ownership; it does not rewrite every
  native KB feature in Go. Code graph and shared corpus operations remain KB
  features. Personal `memory.ask` supplies local retrieval; it is not an automatic
  background synthesis/curation pipeline.
- LUKS requires a Linux Docker host with loop and device-mapper support. The
  documented `scripts/compose-local.sh` discovers the host device major and seals
  credentials before `up`. A fresh raw `docker compose up` bypasses that bootstrap.
- Offline migration currently accepts PostgreSQL 18. The old plaintext source is
  kept read-only for rollback, so migration does not erase pre-existing plaintext.
  Follow the upgrade guide for verification and subsequent operator removal.
- Back up the Vault and encrypted PostgreSQL volume together. Losing the only
  Vault credential makes the encrypted volume unrecoverable.
- Model certificates currently expire after one year and fail closed. Automatic
  model certificate renewal is not implemented. This remains an operational
  release follow-up; it must not be described as unattended indefinite operation.
- Standalone repository exports must build independently and their exact release
  pins must come from published repositories. Local export commits are validation
  artifacts, not evidence of remote publication. The checked-in release lock
  currently fails the source-mirror check and needs legitimate published pins.
  Export CI now restores that lock after generating its local fixtures, so those
  fixtures cannot mask stale release pins. The draft PR does not bypass
  main-merge approval or release gates.

The independent export test found and repaired three packaging regressions: CMake
linker settings differed from the runtime bundle, role entry points referenced a
nonexistent generic handler, and the memory export omitted its egress contract.
The regression compiles an unused legacy reference successfully, then makes that
reference live and requires the linker to reject it. Integration CI now builds
the exported roles and common modules, with published-pin validation still
restricted to release integration.
