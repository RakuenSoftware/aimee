# Modules

Module documents describe ownership and contracts, not every implementation file. The descriptor and
public header remain authoritative.

A module document should cover:

- responsibility and non-responsibility;
- public API and event kinds;
- dependencies and consumers;
- configuration and capabilities;
- data, trust, and failure boundaries;
- required tests and generated/attested status.

Current testing trees generate or attest module pages as decomposition lands. Start with the
[event-bus module](bus.md) and [technical reference](../../src/README.md).
