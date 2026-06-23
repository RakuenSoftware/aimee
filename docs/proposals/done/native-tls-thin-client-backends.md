# Native TLS backends for the Windows and macOS thin clients

- **State:** done
- **Scope:** deterministic / build + client transport. Not an intelligence-surface
  proposal (no Architecture Charter role).
- **Author:** JBailes, 2026-06-17.

## Problem

The prebuilt **Windows** and **macOS** thin-client release binaries are built with
`-DWITH_TLS=OFF` and therefore **refuse `https://` servers**. Only the Linux
prebuilt links TLS (system OpenSSL, `WITH_TLS` defaults to `1` on POSIX in
`src/Makefile:154`). A user who downloads `aimee-windows-x86_64.exe` or
`aimee-macos-universal` and points it at an HTTPS `aimee-server` cannot connect;
the documented stopgap is to terminate TLS at a reverse proxy and use its
`http://` address (`docs/QUICKSTART.md`, "TLS support by build").

The two are off for concrete, *different* reasons — both rooted in OpenSSL
packaging, not in the TLS code:

- **macOS**: the release artifact is a **universal (arm64 + x86_64) fat binary**.
  Homebrew OpenSSL is per-arch (separate arm64/x86_64 kegs), so one OpenSSL cannot
  be linked into a fat build. The release workflow disables TLS specifically to
  avoid arch-specific brew libraries (`.github/workflows/release-thin-client.yml`,
  macOS step comment).
- **Windows**: the MinGW build has no OpenSSL on the runner, so it is built
  `WITH_TLS=OFF` and refuses `https://` instead
  (`.github/workflows/release-thin-client.yml`, Windows step).

## Goal

Ship Windows and macOS prebuilt thin clients that speak `https://` **out of the
box**, with **certificate verification against the OS trust store** and no
configuration, no bundled CA bundle, and no extra runtime dependencies — i.e. the
binary stays a single self-contained file, which is the defining property of the
thin client.

## Secondary benefit: remote vault provisioning

The vault already defines an `ATTEST_TLS_BEARER` transport
(`src/headers/vault_principal.h`): "native-TLS conn authorized by bearer … the
bearer over a confidential channel is the operator's authority → server-principal
writes allowed (native-TLS provisioning); no per-user principal (uses
VAULT_SERVER)." Today no client can exercise it on Windows/macOS because those
prebuilts have no TLS. Shipping native TLS therefore also enables an operator to
provision the server vault **remotely over `https://`** (an `aimee vault
set --server` path), complementing the boot-time delegate-vault provisioning
shipped separately (the auto-vault-provisioning-at-standup work, PR #410).

## Why not just link OpenSSL (rejected alternative)

Keeping OpenSSL and fixing packaging was considered and rejected:

- **macOS**: would require building two per-arch binaries each linking its arch's
  OpenSSL and `lipo`-merging them, or vendoring a universal static OpenSSL. Worse,
  OpenSSL on macOS does not read the Keychain, so `SSL_CTX_set_default_verify_paths`
  finds no trust anchors — we would have to **bundle a CA bundle** (which rots) or
  write Keychain glue anyway.
- **Windows**: static-linking MSYS2 OpenSSL keeps a single exe, but OpenSSL has no
  default trust store on Windows either, so we would again bundle a CA bundle or
  load the system `ROOT` store into an `X509_STORE` by hand.

Net: bigger binaries, a stale CA bundle (or cert-store glue we'd have to write
regardless), and OpenSSL CVE-tracking on two more platforms — for a result that is
still not truly "automatic." If we're writing trust-store glue anyway, the native
backends are simpler and use the OS store directly.

## Design

The TLS surface is already abstracted behind a **4-function interface**
(`src/headers/aimee_tls.h`):

```c
aimee_tls_t *aimee_tls_connect(int fd, const char *host);
int          aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len);
long         aimee_tls_read(aimee_tls_t *t, void *buf, size_t len);
void         aimee_tls_free(aimee_tls_t *t);
```

with a **single consumer** (`src/aimee_client.c`) and one implementation today
(`src/aimee_tls.c`, OpenSSL, 104 lines). Add two more implementations of the same
interface, selected at build time by platform:

| Platform | Backend | Links | Trust store |
|----------|---------|-------|-------------|
| Linux | OpenSSL (`aimee_tls.c`, unchanged) | `-lssl -lcrypto` | system PEM paths |
| Windows | **Schannel (SSPI)** — new `aimee_tls_schannel.c` | `secur32`, `crypt32` (in the Windows SDK; no external libs) | Windows certificate store (automatic) |
| macOS | **Secure Transport** (`SSLContext`) — new `aimee_tls_securetransport.c` | `Security`, `CoreFoundation` frameworks (universal) | Keychain (automatic) |

Backend selection is mechanical: extend the `WITH_TLS=1` branch in the build to
pick the source file and link flags by target OS, replacing the single hardcoded
OpenSSL path. `aimee_client.c` is unchanged — the new backends honor the existing
contract exactly:

- TLS ≥ 1.2 minimum, SNI from `host`, hostname verification on by default.
- The `AIMEE_TLS_INSECURE=1` escape hatch (skip verification for self-signed/dev
  servers) — already honored by `aimee_tls.c:tls_insecure()` — is reproduced in
  both new backends.
- Opaque handle owns the TLS state, does **not** close the underlying `fd`
  (matches the header contract); partial reads/writes and clean shutdown handled.

### Notes on the macOS backend choice

`SSLContext`/Secure Transport is deprecated since macOS 10.15 but still ships and
is the simplest C-friendly TLS API on the platform; the alternative,
**Network.framework** (`nw_connection_t`), is modern but async/block-based and
would not map as cleanly onto the synchronous `connect/read/write` interface.
Recommendation: Secure Transport for v1 (lowest blast radius), with
Network.framework noted as a future migration if Apple removes Secure Transport.
Either way the framework is universal, so the fat-binary problem disappears
entirely — this is the strongest single argument for the native route.

### Release-workflow changes

- macOS job: drop `-DWITH_TLS=OFF` (build `WITH_TLS=ON`); the `CMAKE_OSX_ARCHITECTURES`
  universal build still links because `Security`/`CoreFoundation` are universal.
- Windows job: drop `-DWITH_TLS=OFF`; link `secur32`/`crypt32`.
- Keep the existing `aimee <asset> version` smoke test; add an `https://` handshake
  smoke test against a known-good endpoint per platform.

### Documentation changes

- Update `docs/QUICKSTART.md` "TLS support by build" table: Windows and macOS
  prebuilts move to **Yes**.
- Update the Windows/macOS install notes (and `install.ps1`'s "TLS is off on
  Windows" comment, README/MANUAL "Windows thin client is built without TLS")
  to reflect native TLS.

## Out of scope

- Server/kb TLS termination (they already require OpenSSL; unchanged).
- mTLS / client certificates.
- Replacing the Linux OpenSSL backend.

## Risks and effort

- Two new platform backends (~300–400 lines each) to write and test for **parity**
  with the OpenSSL backend: handshake, SNI, hostname check, min-version, the
  insecure escape hatch, partial I/O, shutdown.
- Per-platform CI must actually exercise an `https://` handshake, not just
  `version`, or a broken backend ships silently.
- Schannel and Secure Transport have fiddly buffering/renegotiation semantics;
  the abstraction is small but the handshake loops need care.
- Blast radius on the rest of the codebase is **nil**: additive new files behind an
  existing interface with a single caller; no change to `aimee_client.c` or the
  Linux build.

## Implementation requirements (must address before coding)

Consolidated from an interim self-review and a 3-lens roundtable (architecture,
security, QA). No design blockers were found; these are the specifics each backend
must implement to match the OpenSSL backend's behavior and security posture.

### A. Security-critical (a miss here is a silent MITM hole)

1. **Schannel does NOT verify the hostname for you.** `InitializeSecurityContext`
   validates the chain but performs **no** hostname check. After the handshake the
   backend MUST call `CertGetCertificateChain` then
   `CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, …)` with an
   `SSL_EXTRA_CERT_CHAIN_POLICY_PARA` whose `pwszServerName` is the connect `host`,
   and fail closed on any non-`SEC_E_OK`/non-zero policy status. This is the single
   most important parity item: without it, any cert from any CA (or a valid cert for
   a different host) is accepted. Use `SCH_CRED_AUTO_CRED_VALIDATION` for the chain,
   not `MANUAL`, in the secure path.
2. **Secure Transport hostname check requires `SSLSetPeerDomainName`** (with the
   connect `host`) **before** the handshake. If it is not set, no hostname check is
   performed. Do not set `kSSLSessionOptionBreakOnServerAuth` in the secure path (it
   suppresses the built-in evaluation); let Secure Transport evaluate trust + name.
3. **Protocol floor: disable TLS 1.0/1.1.** Schannel: set
   `grbitEnabledProtocols = SP_PROT_TLS1_2_PLUS` (and TLS 1.3 where available) in the
   `SCHANNEL_CRED`/`TLS_PARAMETERS`. Secure Transport: `SSLSetProtocolVersionMin`
   `kTLSProtocol12`. Matches the OpenSSL backend's `TLS1_2_VERSION` floor.
4. **`AIMEE_TLS_INSECURE` semantics + blast radius.** Read it via `getenv` **at
   connect time** (per-connection, exactly like the OpenSSL backend's
   `tls_insecure()` — not a compile-time flag, not a one-shot startup read). When
   set it disables **all** verification — chain AND hostname — so the proposal must
   document it as a dev-only MITM-accepting switch. Insecure path per backend:
   Schannel `SCH_CRED_MANUAL_CRED_VALIDATION` + skip the policy check; Secure
   Transport `kSSLSessionOptionBreakOnServerAuth` + accept without evaluating. Secure
   default = full verify; insecure = full skip; no middle state.

### B. Correctness / parity (a miss here is a hang, truncation, or crash)

5. **The 4-function interface hides record framing.** Neither API transparently
   wraps a raw `fd`. Schannel needs an explicit `InitializeSecurityContext` handshake
   loop + `EncryptMessage`/`DecryptMessage`, and `DecryptMessage` returns leftover
   *ciphertext* in a `SECBUFFER_EXTRA` that must be carried into the next read.
   Secure Transport drives raw I/O through `SSLSetIOFuncs` callbacks. Therefore
   `struct aimee_tls` MUST hold **two** carry-over buffers: decrypted-but-unconsumed
   **plaintext** (when the caller's `read` buf is smaller than a record) and
   leftover **ciphertext** (`SECBUFFER_EXTRA`). `aimee_tls_read` drains plaintext
   first, then decrypts more. This is the main parity risk.
6. **Mid-stream renegotiation.** `DecryptMessage` can return `SEC_I_RENEGOTIATE`
   (and `SEC_I_CONTEXT_EXPIRED` on close); the read path must loop back through the
   handshake on the former and report clean EOF on the latter. Secure Transport
   surfaces `errSSLPeerAuthCompleted`/renegotiation similarly. Do not treat these as
   errors.
7. **Blocking-socket semantics + clean shutdown.** `aimee_client.c` uses a blocking
   socket, one TLS handle per connection, single-threaded per handle (no locking
   needed). Map cleanly: `aimee_tls_read` returns `0` only on a genuine TLS
   close-notify/EOF and `-1` on error (distinguish Secure Transport
   `errSSLClosedGraceful` → 0 vs other `OSStatus` → -1; Schannel `SEC_I_CONTEXT_EXPIRED`
   → 0 vs `SEC_E_*` → -1). `aimee_tls_free` MUST send close-notify and tear down the
   context (`DeleteSecurityContext`/`FreeCredentialsHandle`; `SSLClose` +
   `CFRelease`) but MUST NOT close the underlying `fd` (per the header contract).

### C. Build / CI

8. **`-Werror` + Secure Transport deprecation = build break.** `SSLContext`/Secure
   Transport is deprecated since macOS 10.15 and emits `-Wdeprecated-declarations`,
   which `-Werror` makes fatal. Apply `-Wno-deprecated-declarations` **only to the
   `aimee_tls_securetransport.c` translation unit** (a per-file CMake
   `set_source_files_properties(... COMPILE_OPTIONS ...)`, never globally — that would
   mask other deprecations). Long-term migration to Network.framework noted if Apple
   removes Secure Transport.
9. **CMake is the release build for Win/macOS** (the Makefile is the Linux path).
   Wire backend selection there:
   ```cmake
   if(WITH_TLS)
     if(APPLE)       target_sources(aimee PRIVATE src/aimee_tls_securetransport.c)
                     target_link_libraries(aimee "-framework Security" "-framework CoreFoundation")
     elseif(WIN32)   target_sources(aimee PRIVATE src/aimee_tls_schannel.c)
                     target_link_libraries(aimee secur32 crypt32)
     else()          target_sources(aimee PRIVATE src/aimee_tls.c)        # OpenSSL
                     target_link_libraries(aimee OpenSSL::SSL OpenSSL::Crypto)
     endif()
   endif()
   ```
   MinGW prerequisites for the Schannel TU: `#define SECURITY_WIN32` before
   `<security.h>`/`<schannel.h>` (MinGW-w64 ships both); link `-lsecur32 -lcrypt32`.
   Confirm in CI that the macOS artifact stays a true universal binary (`lipo -archs`).
10. **Tests must catch a broken backend, not just a happy path.** Per-platform CI
    must run, against a **local self-signed server** (deterministic; not a public
    badssl endpoint): (a) good cert → handshake + round-trip; (b) expired cert →
    rejected; (c) wrong-hostname cert → rejected (this is the test that catches the
    Schannel hostname footgun in A.1); (d) `AIMEE_TLS_INSECURE=1` → (a)–(c) all
    accepted; (e) a payload larger than one TLS record + a 1-byte-at-a-time read loop
    to exercise the plaintext/ciphertext carry-over buffers (B.5).

## Acceptance criteria

1. `aimee-windows-x86_64.exe` and `aimee-macos-universal` connect to an `https://`
   `aimee-server` with verification on, no bundled CA bundle, no extra runtime deps.
2. `AIMEE_TLS_INSECURE=1` works identically on all three platforms.
3. Invalid/expired/hostname-mismatched certs are rejected by default on all three.
4. macOS artifact remains a true universal (`lipo -archs` shows arm64 + x86_64).
5. CI runs a real `https://` handshake smoke test per platform.
6. Quickstart/README/MANUAL TLS notes updated.
