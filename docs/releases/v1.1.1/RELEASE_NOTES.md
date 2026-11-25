# Aimee 1.1.1

A patch release in the `1.1` series. It fixes failures found running 1.1.0 as a standalone,
KB-less Server: delegation, webchat, personal memory commands, background delegate sources and
audit startup time. Memory is still served by the vendored aims-db bundle introduced in 1.1.0,
rebuilt here with the retrieval health collector.

## Upgrade

- **Take the new `compose.yaml`.** It adds `aimee-delegate-runtime`, a private Docker-in-Docker
  daemon. Without it a 1.1.x Server has no container runtime and refuses every delegation. The
  service is **privileged**. It listens only on a Unix socket owned by gid 1000, on its own network,
  and only `aimee-server` mounts that socket; delegate containers never do. It mounts the Server's
  home and workspaces at identical paths. `compose.kb.yaml` leaves it out: a KB runs no delegates.
- **The first start after the upgrade builds two audit indexes once.** Receipt lookups read an
  index instead of scanning the whole ledger. Building it takes time in proportion to the audit
  ledger's size, on top of the startup ledger verification. If it fails, the Server logs
  `audit_worm: receipt lookup index failed: ...` and starts anyway, with slower receipt lookups.
- Stored memory, the audit ledger, Vault identity and sessions are kept. No data migration runs.

## Fixes

- **Delegation.** `aimee status` now prints a `delegates:` line reporting the sandbox runtime;
  previously the posture appeared only in the boot log while status said ok. Managed model
  services keep using the host daemon.
- **Background delegate sources.** A first mirror of a large repository no longer times out:
  clone and fetch get 20 minutes instead of 30 seconds. A working-tree patch over the 16 MiB cap
  is no longer shipped truncated. The delegate gets a clean checkout at the pushed base, and the
  CLI says why.
- **Webchat.** A browser tab opened before 1.1.0 has a server session with no owner, and attach
  refused it forever, leaving the Agent picker on "Loading agent…". The first webchat user to open
  such a tab now claims it. An owned session or another client's session is never rewritten.
  Attach and turn errors now name the cause instead of "attach refused".
- **Personal memory.** `aimee memory stats`, `evidence`, `validity` and `health` work on a
  standalone Server. `aimee memory health` reports retrieval health once an operator enables
  collection with `AIMEE_MEMORY_HEALTH_ENABLED=1` (optional `AIMEE_MEMORY_HEALTH_SAMPLE_PPM`). As
  in 1.0 it is off by default. Changing it re-prepares the memory installation on the next start.
- **KB-less Server.** Commands that need a KB say so instead of reporting an outage. `aimee status`
  shows `aimee-kb: not configured (optional)`.
- **Audit.**
  - Startup no longer rescans the whole WORM chain to admit its checkpoint.
  - Verification no longer holds the store lock for the whole scan.
  - Health and receipt inspection check that the ledger was verified rather than verifying it again.
  - A fresh, empty store is admitted as verified.
- **CLI and models.**
  - A credential that cannot be resolved reports why.
  - `aimee primary` outside a session says a session is needed.
  - A known command this Server does not serve says so.
  - `aimee jobs list` keeps its reply inside one ring message instead of failing.
  - Memory recall bounds the `task_hint` it accepts.

## Unchanged

- Synthesis and embedder images are the same digests as 1.1.0 (`dependencies/` is unchanged).
- Server images remain `linux/amd64` only; thin clients are built for every platform.
