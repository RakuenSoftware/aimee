# The ouroboros ring

The local shared-memory transport inside `aimee-server` and `aimee-kb` is the
ouroboros ring. It is a contract of its own, with C, Go and Rust reference
implementations, in
[RakuenSoftware/ouroboros-ring](https://github.com/RakuenSoftware/ouroboros-ring):
what it owns and does not, its requirements (R1 to R6), the wire and segment
specification, the frozen vectors and what conforming means for a new
implementation are all there (`spec/`, `vectors/`).

What stays in aimee:

- **The contract reference.** `dependencies/ouroboros-ring.json` names the
  release range aimee accepts (`compatible`, a semver caret range: `0.1.3` is any 0.1
  release from 0.1.3, the first with `ouro_module_client_pool_t`) and the release builds use
  by default with its content digest. aimee links an installed ring through the
  ring's own package (pkg-config / CMake `ouroboros-ring`) and never builds or
  reads the ring's sources: `scripts/provide_ouroboros_ring.py` takes
  `OURO_RING_PREFIX` (an installed ring, any implementation of the C API),
  `OURO_RING_SOURCE` (a ring checkout, built with the ring's own build) or, by
  default, the named release. `server-go/go.mod` requires a tagged Go release in
  the same range (`make go-unit-tests` checks it).
- **The host's embedding.** `src/modules/audit/obs_bus.c` hosts the ring in each
  daemon: admission grants from `*.grant` files, the tap into audit and
  guardrail stores, capture, and the refused role refs of this instance.
- **Instance roles.** `src/modules/audit/instance_identity.c`: which daemon an
  instance is, the reserved role identities (server 34, KB 35), and the
  first-boot identity latch.
- **The link boundary.** `scripts/check_bus_blast_radius.sh`: only the server and
  KB link `libouroboros-ring.a` ([which binaries link the ring](../EVENT_BUS.md#which-binaries-link-the-ring)).
- **aimee's own budgets.** `scripts/check_audit_bus_budget.sh` holds the audit
  row's enqueue cost to `bench/audit_bus_budget.json`. The ring's dispatch
  budget is the ring's (`scripts/check_perf_budget.sh` there).

What aimee carries on the ring is described in [Event bus](../EVENT_BUS.md).
