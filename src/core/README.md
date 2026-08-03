# Aimee core C libraries

This directory is an extraction-ready C package with two deliberately separate
libraries:

- `aimee-core-connection` is portable inter-process/inter-machine communication.
  Thin client, server, and KB consume the same endpoint, bearer-token, socket,
  deadline/cancellation, HTTP/1, and TLS/mTLS code.
- `aimee-core-event-bus` is the POSIX, local-container shared-memory bus. Server
  and KB each host an independent instance; modules attach through a local
  `SOCK_SEQPACKET` handshake and then use only their shared-memory mappings.
  External module repositories link the smaller
  `aimee-core-event-bus-client` archive, which contains no host or routing code.

Build this package independently with:

```sh
cmake -S src/core -B build/aimee-core
cmake --build build/aimee-core
```

Public headers are namespaced under `aimee/core/connection` and
`aimee/core/event_bus`. The event-bus client depends only on the public attach,
wire, region, and ring contracts. Its archive does not implement the host-only
region lifecycle, admission, routing, arena allocator, or capture APIs.
