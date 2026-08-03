# Core C connection library

`libaimee-core-connection.a` is the shared C connection library. The thin client,
`aimee-server`, and `aimee-kb` all link this archive. Inter-machine traffic uses
this layer; none of the products keeps a private copy of TCP connection setup,
endpoint parsing, HTTP/1 response framing, bearer handling,
cleartext-credential policy, or the OpenSSL TLS/mTLS context and client-session
setup.

The public headers live under `src/core/connection/include/aimee/core/connection`:

- `endpoint.h` parses the HTTP/HTTPS endpoints used between Aimee products.
- `auth.h` formats and parses Bearer values, compares credentials in constant
  time, and owns the fail-closed rule for credentials over cleartext links.
- `socket.h` owns portable DNS/TCP connect timeouts, complete writes, reads,
  controlled deadline/cancellation-aware I/O, readiness waits, and descriptor close behavior. The
  old `platform_net` entry points are compatibility facades over this API.
- `control.h` owns the absolute monotonic deadline, cancellation callback,
  cancellation polling interval, readiness wait, and typed timeout/cancel/I/O
  outcomes used by every controlled connect, TLS handshake, read, and write.
- `http1.h` owns bounded HTTP/1 response reads, `Content-Length` framing,
  status parsing, transfer-encoding rejection, and connection-reuse metadata.
- `tls_openssl.h` owns the OpenSSL client/server context floor and in-memory PEM
  identity/trust loading used by Linux builds, plus client SNI/name
  verification, handshake, complete writes, reads, and session teardown.
- `native_tls.h` owns the common native client API, profile-aware certificate
  path resolution, client identity validation, and its platform backends:
  OpenSSL on Linux, Secure Transport on macOS, and Schannel on Windows.
  `aimee_tls.h` is now only a source-compatibility include for this core API.

The server-to-KB mTLS client, server-to-KB invalidation WebSocket, ordinary
thin-client API client, POSIX thin-client HTTP path, and Windows thin-client
HTTP path consume these same primitives. The agent provider bridge and the
deadline-sensitive KB management and egress clients use the core-controlled
connect/TLS/read/write operations; only resolver/SSRF policy, HTTP parsing, and
product-specific result mapping remain in those callers.

Application routing, certificate enrollment, revocation stores, and durable
identity records remain product-owned policy. They consume the connection
library; they are not transport implementations.

The event bus is deliberately separate. It is not a network transport and is
not linked into the thin client.

The independently installable package is versioned by `src/core/VERSION` and
exports CMake targets under `aimee::`. Consumers require that version exactly. The
monorepo can consume an installed copy with `-DAIMEE_USE_INSTALLED_CORE=ON` or
`AIMEE_CORE_PREFIX=/absolute/prefix`; both paths reject a version mismatch.
