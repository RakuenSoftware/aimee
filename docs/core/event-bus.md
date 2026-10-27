# The ouroboros ring

The local shared-memory transport inside `aimee-server` and `aimee-kb` is the
ouroboros ring. It is a contract of its own, with C, Go and Rust reference
implementations, in
[RakuenSoftware/ouroboros-ring](https://github.com/RakuenSoftware/ouroboros-ring):
what it owns and does not, its requirements (R1 to R6), the wire and segment
specification, the frozen vectors and what conforming means for a new
implementation are all there (`spec/`, `vectors/`).

What stays in aimee:

- **The pin.** `dependencies/ouroboros-ring.lock.json` names the ring commit and a
  digest of its content. `scripts/fetch_ouroboros_ring.py` fetches and checks it
  before anything compiles, and `server-go/go.mod` requires the Go module at the
  same commit (`make go-unit-tests` checks the two agree).
- **The host's embedding.** `src/modules/audit/obs_bus.c` hosts the ring in each
  daemon: admission grants from `*.grant` files, the tap into audit and
  guardrail stores, capture, and the refused role refs of this instance.
- **Instance roles.** `src/modules/audit/instance_identity.c`: which daemon an
  instance is, the reserved role identities (server 34, KB 35), and the
  first-boot identity latch.
- **The link boundary.** `scripts/check_bus_blast_radius.sh`: only the server and
  KB link `libouroboros-ring.a`.

What aimee carries on the ring is described in [Event bus](../EVENT_BUS.md).
