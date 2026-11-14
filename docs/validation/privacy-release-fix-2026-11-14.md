# Vault intake and retained history — 2026-11-14

The corrected core application passed all twelve canonical release gates in a
fresh run (`release-r18`) on owned CT 9474 on 192.168.1.253. All 58 CI checks pass at `d61d31ef4`; the owned test container and its root disk
have been removed. This is a core/CLI and API
qualification; optional runtime-web and VS Code add-ons were disabled in the
local image. No full browser UI qualification or release publication is claimed.

## Fixes

Recognized PII enters authenticated encrypted Vault custody and memory holds an
opaque reference. The exercised writes are accepted; a refusal alone is not a
passing privacy result. Replacement, retirement, and rotation retain ciphertext
history. Missing initialized master keys fail closed without reminting keys.

Phone recognition preserves opaque alphanumeric/underscore identifiers while
still sealing explicit phone numbers. JSON integer tokens remain exact. The
Native fact worker grounds complete Vault identifiers, rejects invented first
person and partial locator labels, and revalidates retained interpretations
without erasing their history. Grounded worker candidates remain available
through configured hybrid retrieval.

Personal ingress now reads its authenticated User collection with or without the
optional original-input adapter. It does not request a typed preview from the
remote KB and compare that different collection against personal memory. Native
fresh-selection, disclosure, source-release, and receipt guards remain enabled.
Preview/final byte allocation also shares the envelope reservation.

CI verdict artifacts now export only reviewed assertions, counts, and hashes;
source contracts, attestations, receipts, environments, and raw failure payloads
are excluded. Terminal recovery failures receive bounded hash-only diagnostics.

## Validation

- All 12 release gates pass: privacy custody; KB and Server with both pinned real
  E4B/E2B models; T1/T2/T3; real Docker private verifier; egress/Vault; ordinary
  PostgreSQL; and current-layout KB upgrade.
- Privacy custody: 90/90. Every exercised PII value was actually decrypted from
  authenticated ciphertext; SQL, persisted files, and captures contained no raw
  canary. Real E4B embedding ingress on both roles had positive calls and zero
  raw person/phone canary matches.
- Matched backup, restore, missing-key refusal, and original-key recovery:
  56/56 on the exact final image. Original memory and versions were preserved.
- T2 and T3 provider boundaries: 353/353 each. Native asynchronous gates:
  95/95 and 87/87 respectively. T1 index/outage assertions: 34/34.
- Native library: 1,053 pass, 79 explicitly ignored by that suite. Context and
  worker integration suites: 13/13 and 19/19. Focused personal-routing and source
  guard tests: 19/19. Public lint: all 76 checks pass.
- An additional full asynchronous run and five focused indexed-recovery probes
  pass. The probes used the owned diagnostic fixture, not a qualification image.
- Earlier real PostgreSQL Vault retention/rekey, full Go PostgreSQL/workflow race,
  authentication retention-reference, and hardened helper sanitizer replays pass;
  their source/log hashes are retained in the local evidence archive.

The recorded two-worker orchestrator validates its gate list against the
canonical script. E4B additionally observes actual embedder ingress and submits a
phone fixture; E2B retains its owned containers for diagnosis. No refusal is
counted as a positive model result. Novel predicate promotion still requires
three grounded distinct notes; model variability is handled by a bounded corpus.

## Provenance and limits

The final image is `aimee:privacy-fixed-r17`, reports
`aimee v1.1.0-privacy-fix`, and has image ID
`sha256:b5869dabd7bc32a1c080159b10dfb321698ed32e4b263ab0c35719e712c8dc38`.
Runtime source is public commit `60ffbd60e`, private commit `c9b34f5`;
subsequent test/export changes do not alter production source or this image.
Eight changed runtime files, 57 public snapshot files, and all seven raw
executables were hash-compared with the test guest. The guest is a base checkout
plus enumerated overrides, not represented as a clean checkout of those commits.

Three Rust executables were rebuilt for the final revision; four executables had
unchanged source. The C bridge archives were reused with all 79 public dependency
fingerprints verified unchanged. The exact image, model, compiler, bundle, source,
and binary hashes accompany this record in the sanitized JSON/local archive.

Local gates use ordinary PostgreSQL and CPU model images. LUKS CI results are
reported separately. Synthetic fixture coverage does not establish detection of
every possible PII spelling or an exhaustive qualification of every feature.

## Negative history and environment

Older candidates remain recorded. Wrong-version images, model publication misses,
invalid opaque endpoints, byte-allocation differences, and the cross-collection
preview defect are superseded by explicit fixes and passing fresh tests.
A 310-second continuation probe excluded simple ticket expiry as the explanation.

The initial final-image matrix exhausted its 150 GiB disposable root filesystem;
that run is invalidated. The verified owned disk was expanded to 230 GiB with
ample physical storage available, and the complete matrix was repeated. Earlier
Docker address-pool exhaustion was handled by removing only completed owned test
containers/networks; retained volumes and recovery fixtures were preserved.

One earlier CI indexed-recovery attempt at `60ffbd60e` stopped after one provider
request without sufficient terminal diagnostics. It did not reproduce in the
complete local run, an additional full asynchronous run, five focused probes, or
subsequent CI T2. Its negative verdict is retained; its original cause is not
claimed solved by instrumentation. Future failures retain bounded terminal
lengths/hashes. The subsequent full CI run `43645850452` passes all 58 checks at `d61d31ef4`,
including T2, T2 with LUKS, T3 and hardened Vault egress.

## Evidence and cleanup

Only explicit verdict/count/hash fields are exported. The export has zero raw or
escaped matches against 42 private fixture values; guest/host hashes match and
secret scanning passes. The final archive excludes raw canaries, keys, controller state, native frames,
and backup archives. Those guest-only files were removed with the disposable
container after export verification. The new CI artifact was downloaded and its
whitelist structure verified; native attestation payloads are excluded.

CT 9474 (`aimee-privacy-release-fix`) was identity-checked, stopped and destroyed
at `2026-11-14T22:33:38Z`. Its Proxmox configuration and owned root volume
`storage:vm-9474-disk-0` are absent. CT 9473 remained running before and after.

[Sanitized assertions, provenance, negative history and cleanup](privacy-release-fix-2026-11-14.json)
record the exact evidence. The PR remains open against `testing`; no merge or
release publication was performed.
