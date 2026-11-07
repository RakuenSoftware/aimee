# Aimee core C libraries

This directory is an extraction-ready C package with shared data contracts and two
separate libraries:

- `aimee-core-codec` provides portable byte codecs. Base64 callers explicitly choose
  canonical padded standard, canonical unpadded URL, or an existing compatibility
  profile. Authentication uses the canonical URL profile; text framing and Vault
  records retain their separate historical acceptance rules.

- `aimee-core-types` provides data-only panel contracts shared by IR, delegates,
  and roundtable without peer-service dependencies.
- `aimee-core-connection` is portable inter-process/inter-machine communication.
  Thin client, server, and KB consume the same endpoint, bearer-token, socket,
  deadline/cancellation, HTTP/1, and TLS/mTLS code.

The local shared-memory ring that server and KB host is not part of this package:
it is the ouroboros ring (github.com/RakuenSoftware/ouroboros-ring), an
installed release in the range `dependencies/ouroboros-ring.json` names.

Build this package independently with:

```sh
cmake -S src/core -B build/aimee-core
cmake --build build/aimee-core
```

Public headers are namespaced under `aimee/core/connection`,
`aimee/core/codec`, and `aimee/core/types`.
