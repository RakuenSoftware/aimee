# Proposal: Config changes take effect immediately (live reload)

- **State:** ✅ **CORE COMPLETE — all core phases merged to `testing` (closed out
  2026-07-04).** The central goal — a config change takes effect **immediately** on the
  running server, with the few genuinely restart-bound settings surfaced honestly — is
  delivered and **live-verified**. See **Implementation status** below for what shipped and
  the carried follow-ups.

## Implementation status

| Phase | PR | Result |
| --- | --- | --- |
| Proposal | #1056 | This doc — 2 roundtable rounds, no blocking. |
| **P1a** | #1057 | Reload core: config-snapshot **double-buffer + seqlock**, `config_reload()` with **validate-or-keep** + a content-hash no-op guard (torn-read stress-tested). Also fixed a pre-existing bug: `economizer.*` was ignored when a config had no `reduce:` block. |
| **P1b** | #1059 | **Wired + live-verified:** `config set` → the same running server reflects it **immediately**; **SIGHUP reloads** instead of shutting down; `config_load` transparently returns the live snapshot in the server (disk-fresh read-modify-save for `config.set`), which also closed the pre-existing `g_config_cache` reader race. |
| **P2** | #1062 | **Honesty layer:** `reload_class` (`hot`/`reappliable`/`restart`) per field + a **Live / Restart** verdict on `config set`/Settings. Table audited (endpoints/models/flags are per-request = HOT; the startup-bound minority `db2_url`/`kb_api_*`/`autonomy.*` = RESTART). |
| **P3** | #1063 | **Re-applier registry** — a `hook(old, new)` invoked after a changed reload, the foundation for making bound state live. |
| **P5** | (this) | Close-out. |

**The core win:** aimee's config changes now take effect immediately for the per-request
path (the majority of settings), and the user is told **Live** vs **Restart** for every
`config set` instead of guessing. Notably, the P1a parse-bug fix alone already resolved the
specific staleness that motivated this proposal (`economizer.aggressive`).

**Carried follow-ups** (each a genuine, separate effort — not loose ends):
1. **TLS cert live reload.** The TLS/`aimee.api.*` fields are **not** in the `config.set`
   allowlist (only `kb_api_*` is), so there is no `config.set` trigger; live TLS reload is a
   distinct SIGHUP-driven cert-re-read + listener `SSL_CTX` swap effort, using the P3 registry.
2. **`autonomy.*` live** — reverted the env-`setenv` re-applier as a **POSIX/glibc data race**
   against wfe's per-run `getenv`. Making it live safely needs wfe to read the thread-safe
   snapshot instead of env — a wfe-boundary redesign (wfe is deliberately decoupled from
   `config.h`). Stays `RESTART` until then.
3. **Bearer-token rotation / plugin re-parse** — further P3 re-applier consumers.

The proposal's original P3–P5 (re-appliers for every startup-bound field, TLS, close-out) is
narrowed by reality: most fields were already made live by P1b (per-request readers), so only
the small startup-bound minority needs re-appliers, and those (TLS, autonomy-via-wfe) are the
carried efforts above.

---

- **Original design (as proposed):** A config change made via `aimee config set`
  or the web **Settings** page should take effect **immediately** on the running server, not
  "on the next server start." Where a setting genuinely cannot be re-applied live, the server
  says so **explicitly** at change time instead of silently deferring to a restart.
- **Thesis:** aimee's effective config behaviour today is **mixed and unsurfaced**.
  `config_load` is mtime-cached, so consumers that read it **per request** (the economizer
  `reduce.*` levers, and similar) *do* pick up a change on their next request — that path is
  already live. But a large, important class of settings is **bound once at server startup**
  and needs a restart: the inbound `/v1` listener (`aimee.api.http_port` / `tls_port` /
  `bearer_token`), the `autonomy.*` knobs (bridged to env once via `autonomy_config_to_env`),
  plugin extensions (`config_load_plugin_extensions`), log level, and the vault. The user
  can't tell which class a given knob is in, so the safe mental model became "restart to be
  sure." This proposal makes the default **immediate**, adds a **push** signal so a change
  doesn't wait for a cache miss, and makes the **restart-required** minority explicit.

## §0 The current model (verified)

| Path | How it reads config | Effect of a change |
| --- | --- | --- |
| Economizer `reduce.*`, per-request `config_load` callers | mtime-cached `config_load` each request | **Live** on the next request once the mtime changes |
| `aimee.api.*` listener (port/TLS/bearer) | `server_http_start(cfg…)` once at startup | **Restart** — the socket/cert/token are bound at listen time |
| `autonomy.*` | `autonomy_config_to_env(&cfg)` once at startup (setenv, no-overwrite) | **Restart** — the wfe library reads env at process start |
| Plugin extensions | `config_load_plugin_extensions(&cfg)` once | **Restart** |
| Log level | bound at startup | **Restart** |
| Vault / master key | opened at startup | **Restart** (by design) |

Two gaps: (1) even the *live* path waits for the mtime cache to miss (no push), and the
same-second write/read can return stale; (2) the *startup-bound* class silently needs a
restart with no signal to the user.

## §1 Goals / non-goals
- **Goal:** a `config set` / Settings save takes effect immediately for every setting that
  *can* be re-applied live, and returns a clear **"needs restart"** for the few that can't.
- **Goal:** no polling — the running server is *told* to reload, so effect is immediate.
- **Non-goal:** hot-swapping things that are genuinely unsafe to change live — the listen
  **socket path**, the **vault master key**. These stay restart-required, surfaced honestly.
- **Non-goal:** a new config format or schema — this is about *when* changes apply.

## §2 Design

0. **Precondition (verified): `config_t` is a flat POD.** It holds only ints and fixed
   `char[]` arrays — no heap pointers — which is why `config_load` already `memcpy`s the whole
   struct into/out of `g_config_cache` and callers pass their *own* `config_t` buffer and get
   a **value copy**. So the reader contract is *already* copy-out (no caller holds a pointer
   into the cache across a call), and the struct is trivially double-bufferable. P1
   carries an audit confirming no consumer stashes a `config_t*` (or a `char*` into one) past
   the `config_load` call.
1. **One reload entrypoint — `config_reload()`.** Re-reads the file, validates it (the same
   `config_reduce_validate`-style gate, so a bad config is *rejected* and the live config
   kept), and publishes a new snapshot into a **double buffer** (two fixed `config_t` slots —
   no malloc, no free, no grace period) guarded by a **seqlock**: the writer fills the
   inactive slot, bumps a version counter odd, points the active index at it, bumps even;
   readers load the counter (acquire), copy the active slot, re-load the counter, and **retry
   if it is odd or changed** — so a reader always gets one coherent snapshot with no
   per-request lock and nothing to free. `config_reload()` returns the set of keys that
   changed (semantic diff, see §2.5).
2. **Push, not poll — `/v1` primary, signal secondary.** The **primary** trigger is a
   first-class `/v1` `config.reload` op that `config set` and the Settings save handler call,
   so effect is immediate and works under every supervisor and in headless/cron runs.
   `SIGHUP` is a **best-effort secondary** (only acts when the process is the direct signal
   recipient; swallowed/misrouted under systemd/docker/runit multi-process — documented per
   supervisor, with a debug log on every SIGHUP received). No consumer waits for a cache miss.
3. **Re-applier registry.** Consumers that hold *derived or bound* state register a re-apply
   hook keyed by the config sections they depend on; `config_reload()` invokes the hooks
   whose keys changed. Concrete re-appliers:
   - **`autonomy.*`** — stop bridging to env at startup only; re-apply on reload (or, better,
     have the wfe bridge read the live snapshot each run so no env round-trip is needed).
   - **log level** — set the live logger threshold.
   - **`aimee.api.bearer_token`** — rotate the accepted bearer live (no re-bind needed).
   - **TLS cert/key** — reload the cert into the live listener (SNI/ctx swap) without dropping
     the socket.
   - **plugin extensions** — re-parse.
4. **Classification is data, not tribal knowledge.** Each config field carries a
   **reload class** — `hot` (per-request), `reappliable` (has a hook), or `restart` — in the
   field table (`config_fields[]`). `config set` / Settings uses it to tell the user exactly
   what happened: *"applied live"* vs *"needs a restart to take effect."*
5. **Cache correctness = a content-hash change-token.** Drop mtime-alone (same-second writes
   are invisible) and sidecar/xattr schemes (atomicity + portability failure modes). The
   change-token is a **hash of the parsed, normalized config struct** (semantic, not textual).
   This makes (file, token) consistent *by construction*, is consistent by construction, and gives a
   free **self-reload no-op guard**: if a reload would produce the same *logical* state as the
   current snapshot (e.g. `config_save` re-normalized whitespace/key-order), the token is
   unchanged → skip the swap and skip re-appliers, so a normalizing save never re-runs an
   expensive re-applier (plugin re-parse). `st_size`+`st_mtim` may be kept only as a fast-path
   hint before hashing.
6. **Two-stage commit + per-section failure semantics.** Reload is a *prepare-then-publish*:
   (a) build a **candidate** `config_t` from the validated file; (b) run each changed
   section's re-applier against the candidate — a re-applier that **fails writes the section's
   PRIOR values back into the candidate** (revert-in-place) and records a per-section error;
   (c) only the *resolved* candidate is published via the seqlock swap. So readers only ever
   see old → resolved-new, never an intermediate. A validation failure at (a) publishes
   nothing (running config untouched). A re-applier failure at (b) flips an operator-visible
   **degraded** flag for that section but the reload **still succeeds for unrelated sections**
   — a bad cert never downs the server or blocks a log-level change in the same save.
   Crucially, the stored change-token reflects the **resolved live state** (with reverts), not
   the file — so a still-broken section differs from the file's logical state and is
   **retried on the next reload** rather than suppressed as "already seen."
7. **One process, many threads — no fan-out needed.** aimee's server is a single process with
   a pthread pool (agent-parallel, compute-pool, HTTP workers) — **not** a prefork/multi-process
   model, and there is **no cross-host shared config** (each host is independent). So the
   double-buffer + seqlock live in **ordinary process memory**: every reader thread shares it,
   a single writer's swap is seen by all, and there is no shared-memory segment, no IPC
   fan-out, and no cross-process/cross-host coherence problem to solve. (This also drops the
   segment-perms/secret-exposure concern — there is no second process to leak to.)

## §3 UX
- `aimee config set K V` prints: `applied live` | `applied live (took effect on N in-flight
  consumers on next request)` | `saved — needs a server restart to take effect` per the key's
  reload class.
- The web **Settings** page shows a per-row badge (**Live** / **Restart**) and, on save,
  a toast reflecting what actually happened.
- `aimee config reload` (and `SIGHUP`) force a full reload on demand.

## §4 Phased plan (each roundtable-gated + shippable)
- **P1 — the reload core.** `config_reload()` + seqlock in-process double-buffer + content-hash
  token + self-reload guard + validate-or-keep + the `/v1` `config.reload` op (+ best-effort
  `SIGHUP`); `config set` triggers it. Fixes the *hot* path's push + same-second staleness for
  all reader threads. No behaviour change for startup-bound keys yet.
  *Tests:* (a) N concurrent `config_set` + `config_load` under load → no reader sees a torn
  snapshot (seqlock retry exercised); (b) `kill -9` between `config_save` and the reload
  signal → next reader/reload recovers a coherent config; (c) an **invalid** written config →
  reload rejected, the running config unchanged; (d) a normalizing re-save → token unchanged →
  no-op (re-appliers not re-run).
- **P2 — reload classification.** Add `reload_class` (`hot`/`reappliable`/`restart`) to
  `config_fields[]`; surface Live/Restart in `config set` + Settings. Honest immediately, even
  before every re-applier exists. *Tests:* each field has a class; `config set` prints the
  right verdict for one field of each class.
- **P3 — re-appliers, highest-value first.** log level; `aimee.api.bearer_token` rotation;
  plugin re-parse; and **`autonomy.*`** in two sub-steps: **P3a** a wfe-call-site shim that
  reads the live snapshot and pushes env at each call (feature-flagged) with a **regression
  test that changes a knob and observes a behaviour change WITHOUT restart**; **P3b** drop the
  startup env bridge only *after* confirming the wfe library does not cache env at init (if it
  does, P3a's live-read gives false confidence — the shim must re-push per call). *Tests:* one
  reload-and-observe test per re-applier; a re-applier-failure test (one section fails → it
  reverts + degraded, others still apply).
- **P4 — TLS live reload.** cert/key swap into the live listener (ctx/SNI swap) without
  dropping the socket. *Tests:* reload a new cert → new connections use it, in-flight
  connections unaffected; a bad cert → section reverts, listener stays up on the old cert.
- **P5 — close-out.** Only the socket **path** and the **vault master key** remain
  restart-required, documented as deliberate.

## §5 Open items (for roundtable)
1. The P1 pointer audit (§0): enumerate `config_load` call sites and confirm none retains a
   `config_t*`/`char*` into the cache past the call (the signature forces copy-out, so this is
   expected to be clean — but confirm, and consider an opaque-handle guard to keep it clean).
2. The wfe env-caching question (§P3a/P3b) needs a concrete probe before P3b commits — does the
   wfe library read env per call or cache it at init?
3. Seqlock reader cost: `config_load` is called per-request on hot paths; confirm the
   copy-the-struct-under-seqlock cost is negligible vs today's mtime-cached memcpy (it should
   be — same memcpy, plus two relaxed counter loads).
