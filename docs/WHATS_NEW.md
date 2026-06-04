# What's New

Each section covers one release. The CLI reads the section matching the
current version and prints it once after an upgrade.

---

## v0.2.0

- **Docker-first deployment**: run `aimee-server` + `aimee-kb` as containers, either one
  combined image (`compose.combined.yaml`, recommended) or split
  (`compose.server.yaml`), and install only the thin client on each developer
  machine. See the README "Run in Docker" and Manual §27.1.
- **Cross-platform thin client**: the `aimee` CLI now drives a remote server over
  HTTP from Linux, macOS, and Windows. Point it with `--server` /
  `AIMEE_SERVER_URL` (+ `--server-token` / `AIMEE_SERVER_TOKEN`), or persist it
  with `aimee remote set`. Prebuilt thin-client binaries ship with each release;
  build just the client with `-DAIMEE_THIN_CLIENT=ON`.
- **KB is HTTP-only**: `aimee-server` reaches `aimee-kb` exclusively over its `/v1`
  HTTP API (`AIMEE_KB_API_URL`); the legacy Unix-socket KB transport and on-demand
  autostart were retired. Deploys must run `aimee-kb --http-port=8741` (or its
  service unit / container).
- **Self-update notifier**: aimee now tells you what changed after each upgrade
  and warns when the binary is older than the source tree.
- **Stale binary detection**: a one-line warning appears when the binary predates
  the latest commit. Run `make` to rebuild.
- **Lean build profile** (`make lean`): a size-optimised, stripped binary pair for
  minimal deployments, capped at 1.5 MB.
- **Autonomous mode**: drive work without per-step prompts via `aimee auto` (roadmap dispatch loop) and `aimee autopilot` (end-to-end pipeline).
- **Slash command registry**: extensible `/command` system in aimee chat.
- **Event notifications**: fired at delegation and verification boundaries.
