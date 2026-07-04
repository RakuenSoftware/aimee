# What's New

Each section covers one release. The CLI reads the section matching the
current version and prints it once after an upgrade.

---

## Unreleased (testing)

Thin-client + credential hardening, the client owns the working tree; agent
credentials live in the server's **sealed vault**; see
[THIN_CLIENT.md](THIN_CLIENT.md).

- **Web Settings page for the full typed config**: the web UI **⚙️ Settings** page
  (`/settings`) edits every allowlisted runtime config field — grouped, filterable, with
  per-field save/reset — over the same `config.show`/`config.set` surface as the CLI. Newly
  exposed groups: the context-economizer levers (`reduce.*`) and the autonomous-development
  pipeline knobs (`autonomy.*`, previously environment-only; a change applies on the next
  server start, and an exported `AIMEE_AUTONOMY_*` still overrides). Secrets and endpoints
  are deliberately not in the allowlist. See [SETTINGS.md](SETTINGS.md).
- **Tool-output condensation** (default-off, `reduce.command_filter`): a deterministic,
  command-aware context-economizer lever that condenses recognized command output at the
  delegate tool seam — test-runner failures and compiler diagnostics kept, passing
  transcripts and build progress dropped — with the full output spilled for lossless
  recovery. Fail-open and byte-identical when off. Toggle it in the web Settings page. See
  [features/tool-output-condensation.md](features/tool-output-condensation.md).
- **Self-hosted GPU inference tiers**: the `aimee-kb` inference image ships three tiers you
  swap with one plugin image. `aimee-kb-cpu` runs retrieval on any host, `aimee-kb-gpu-small`
  bakes a Gemma 4 12B synth, and `aimee-kb-gpu-mid` bakes a Gemma 4 26B-A4B synth that fits a
  24 GB card fully resident. The GPU tiers build on a Mesa 25 base so RADV uses the RDNA3
  matrix cores. The local synth also registers as a free delegate. See
  [AIMEE_KB_SYNTH_TIERS.md](AIMEE_KB_SYNTH_TIERS.md).
- **Cross-repo code graph**: the symbol and call graph resolves dependency edges across the
  repositories in your workspace, so blast radius and caller lookups cross repo boundaries.
  Ask in three directions: what a project depends on, what depends on it, or both. See
  [CODE_INTELLIGENCE.md](CODE_INTELLIGENCE.md).
- **Structured-PDF tables, visual crops & OCR** (default-off, opt-in per layer):
  on top of the coordinate-anchored PDF spine, aimee-kb can now embed PDF chunks
  into a structurally-isolated vector relation with a per-query answerability
  signal (`kb_pdf_vector_enabled`), recognise table cells via an optional TSR
  sidecar (`kb_pdf_tsr_enabled`, surfaced by `pdf_lookup_table`), render
  figure/table/page crops into a content-addressed blob store served by the
  access-gated, audited `pdf_open_asset` (`kb_pdf_assets_enabled`), and OCR
  scanned PDFs through the same citation path with an asset-only fallback
  (`kb_pdf_ocr_enabled`). Each layer degrades cleanly when its sidecar/binary is
  absent. See [STRUCTURED_PDF.md](STRUCTURED_PDF.md).

- **Server-sealed credential vault (single store)**: agent/delegate API keys and
  Codex/OAuth tokens are sealed in the server's vault, encrypted at rest, keyed
  by agent, and decryptable by the server autonomously (a dual-access wrap, no
  interactive unlock). `aimee agent add … --key K` seals `K` into the vault;
  plaintext storage is refused. Every turn resolves the credential from the vault
  (the turn's attested principal, falling back to the server principal). The
  legacy **client-held keyring and the RAM per-session push (`POST
  /v1/session/credentials`) are gone**. Migrate any leftover
  `~/.config/aimee/agent-keys.json` with `aimee agent key import` (which scrubs the plaintext copy by default; `--keep` to retain it).
- **Codex OAuth in the vault**: `aimee agent setup codex-oauth` runs the
  server-hosted OAuth flow and seals the token into the vault; a Codex agent then
  authenticates server-side as a primary provider or delegate, with no
  per-session push from the client. A legacy plaintext token is migrated and
  scrubbed on first use.
- **Workspaces ingested from the client**: `aimee workspace add <path>` resolves
  the path locally, registers it as `detached`, and pushes the files to the
  server (`POST /v1/index/ingest`, chunked under aimee-kb's body cap), the
  server never reads the client filesystem. `aimee index scan [path]` re-pushes.
- **Claude runs on the thin client (standard `claude` CLI over tmux)**: a
  `--provider claude` agent runs the standard `claude` CLI in a tmux session, it
  needs the `claude` binary, tmux, its login, and the working tree, none of which
  exist on a remote/containerized `aimee-server`. When the active workspace is
  `detached` (a thin client is serving it), aimee now runs that tmux session **on
  the client** by marshalling its tmux commands over the existing runner reverse
  channel, against the client's tree with the client's `~/.claude` login. No
  Claude credential is sent to or stored on the server, and `claude -p` print
  mode is not used. Co-located deployments are unchanged. See
  [DELEGATES.md](DELEGATES.md).
- **Claude via the CLI is primary-only by default**: Claude run via the `claude`
  CLI / tmux login (authenticated by the Claude subscription login, not an API
  key) can be your interactive primary but is **not** usable as a delegate unless
  you opt in with `aimee config set claude_cli_delegate_enabled true`. Driving a
  personal Claude subscription as an automated delegate may violate Anthropic's
  terms and risk account action; enabling the flag prints that warning once. This
  gate is Claude-CLI-specific, all other agents (API-key/HTTP, and other CLI
  agents like the Codex CLI) are unaffected.
- **Attention guard inert by default**: recursive raw scans flow freely unless a
  positive `ingress_max_raw_scans` cap is configured; the destructive-file guard
  is unchanged. The Claude Code integration also re-points stale hook/MCP command
  paths to the installed binary on reinstall.
- **`AIMEE_API_REMOTE_WRITES` env** lets a containerized server set its TCP write
  posture (`off|data|full`) without a writable `aimee.yaml`.
- **Fixes**: remote `/v1/index/scan` (and `/v1/index/ingest`) no longer fail with
  "rpc produced no response" (synchronous handlers under the op-run worker);
  `aimee agent add … --provider codex` is accepted as an alias for the Codex
  adapter.

Delegation defaults and the roundtable, see [DELEGATES.md](DELEGATES.md).

- **Delegates work out of the box**: a default agent roster, the ensemble panel,
  and `remote_writes=full` ship seeded, so `aimee delegate …` and the roundtable
  run on a fresh install with no setup.
- **Roundtable runs through the delegate core**: `aimee delegate aggregate`
  (mixture-of-agents) and `aimee delegate roundtable` fan out to a panel and an
  aggregator synthesizes one answer; the run executes through the delegate path,
  so it stays inside session state, cost accounting, and the audit trail. Cost is
  folded onto the originating session.
- **Use delegates, not agents**: the host AI's own sub-agent tool is blocked
  (Claude Code's `Task` included), always on. The block points the agent at
  `aimee delegate <role>` / `aimee delegate roundtable … --mode review`.
- **Output limits come from the model**: token caps are derived from the model
  registry instead of a hardcoded 4096, so large-context models use their real
  ceiling.
- **`ensemble.max_cost_usd` is optional**: a per-run cost cap is now opt-in,
  unset (or 0) means no limit, the default. Set a positive value to cap a run.

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
