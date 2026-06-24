# Proposal: Embedder runtime model fetch + auto-dimension

- **State:** ✅ **SHIPPED / CLOSED (2026-06-24).** The proposal's goal — runtime
  model fetch + a KB that derives `embedding_dim` from the running embedder so the
  two can never drift — is achieved: §1 (thin runtime-fetch image), §2 dim-drift
  refusal (#337), §2a recorded-dim precedence (#604), and §2b fresh-DB probe (#661)
  are all merged to `testing`. The lone residual, §2c (opt-in auto-reembed), is
  **deferred, not a safety gap** — §2's refuse-and-instruct is the safety floor and
  ships; the re-embed itself is now the operator-gated drop-and-rebuild in
  [`docs/runbooks/unified-llm-cutover.md`](../../runbooks/unified-llm-cutover.md) §4.
  Roundtable 2026-06-14 (security · architect · QA · contrarian, 4 rounds to convergence).
- **Implementation status (2026-06-21):** PARTIAL. §1 (thin image / runtime
  model fetch) is in tree. §2's safety-critical core — the `kb_meta`
  `schema_embedding_dim` record + dim-drift **refusal** (`db2_embedding_dim_record_or_check`,
  atomic upsert) — landed in **PR #337**. **§2a (recorded-dim precedence)
  landed in PR #604:** the recorded dim is now a *source*, not just a guard —
  `db2_embedding_dim_get` + `db2_effective_dim(pinned, configured, recorded)` wire
  the `operator-pin > recorded > (probe)` precedence at the `db2_init` schema-apply
  boundary, so an unpinned populated DB self-derives its dim instead of refusing on
  the default. See [the §2a plan](embedder-runtime-fetch-autodim.plan.md).
  **§2b (fresh-DB probe) landed in PR #661:** the `probed` precedence rung is now
  wired — `db2_init` derives a fresh DB's dim from the embedder `/health` probe
  under `pg_try_advisory_lock`, fail-fast + never-poison, via a registered probe
  seam (`db2_set_embedder_probe`, `src/server/embedder_probe.c`, `embed-remote.py
  --dim`). **Validated live** on a real postgres+embedder+kb stack: fresh DB +
  embedder dim=768 → records 768 / `halfvec(768)` (vs the 1024-default baseline);
  operator pin wins; a bad/zero probe dim fails fast and records nothing. The same
  work fixed a latent defect that had silently disabled BOTH §2a and §2b: the
  config default `embedding_dim=1024` made `config_embedding_dim_is_pinned` report
  "pinned" in every deployment — now defaulted to 0 (unset), so "pinned" means the
  operator explicitly set it. **§2c — DEFERRED (closeout 2026-06-24).** The
  double-gated auto-reembed (`kb_reembed_on_dim_change` off-by-default + `--confirm`
  + `/health=maintenance` + count-divergence → degraded) is the one piece never
  built, and it is **not a safety gap**: §2's refuse-and-instruct (never silently
  serve a wrong-dim embedder against a mismatched store) is the safety floor and it
  ships. The *convenience* of an in-product auto-reembed is now served by the
  operator-gated drop-and-rebuild + parity gate in
  [`docs/runbooks/unified-llm-cutover.md`](../../runbooks/unified-llm-cutover.md) §4,
  which the unified-llm-container migration adopted as the chosen re-embed path — so
  the in-product command is **decided-against**, not missing. *User-facing
  mitigation (the known-gap statement):* a non-operator who changes `EMBEDDER_MODEL`
  against a populated DB is never silently broken — they hit §2's **actionable
  refusal** ("recorded dim N ≠ embedder dim M; this is a guided re-embed") at
  startup, and recovery is the documented drop-and-rebuild. The only thing §2c would
  have added is performing that recovery *in-product behind a flag* instead of by the
  procedure; no scenario is left unrecoverable, only less automated. Forward note: the whole
  torch embedder — and with it this proposal's §1 runtime-fetch surface — is slated
  for retirement by unified-llm-container at the operator cutover (runbook §6); the
  §2/§2a/§2b dim machinery is **retained**, since the unified-llm `(model_id,dim)`
  drift guard builds directly on it. Runtime/bootstrap work validated against the
  live embedder+kb stack.
- **Author:** JBailes
- **Date:** 2026-06-14
- **Base:** `origin/main` (v0.2.65). The embedder ships in two **baked** images
  today: `aimee-embedder` (pplx-embed-v1-4b/2560 + ettin-reranker-1b) and
  `aimee-embedder-0.6b` (pplx-embed-v1-0.6b/1024 + ettin-reranker-400m). The
  dimension is carried separately by `embedding_dim` / `AIMEE_EMBEDDING_DIM`.
- **Companion:** the curator container uses the same fetch-once pattern; see
  [curator-llm-backend](curator-llm-backend.md).

## Goal

One small embedder image. The model (and its paired reranker) is **fetched at
runtime, once**, into a persistent cache volume and reused on every restart. The
KB **derives `embedding_dim` from the running embedder** instead of a hand-synced
config value — so the model and the vector-column dimension can never drift apart.

## Why

Two problems with the current baked design:

1. **Huge images, slow releases.** Baking the 4b + 1b weights makes the published
   image multi-GB; every release rebuilds and re-downloads ~8 GB, and a deploy
   pulls it. Two SKUs (4b and 0.6b) double the build/publish cost.
2. **The dim is a footgun.** `embedding_dim` must match the embedder model by
   hand. When they drift — e.g. switching 0.6b→4b, or a recreate that resized the
   `halfvec` columns — the query embeds at one dimension and the corpus at
   another, and search silently returns nothing. (This exact mismatch took the
   .254 KB down until it was traced; see the kb-search builtin/dim fixes.)

Runtime fetch fixes (1); auto-dimension fixes (2).

## Design

### 1. Thin image, fetch-once into a persistent volume

`Dockerfile.embedder` ships only the runtime (Python + sentence-transformers /
torch, or llama.cpp for GGUF) — **no `RUN … SentenceTransformer(MODEL)` build
step**. The entrypoint, on start:

- If the model is already in the cache volume (`HF_HOME=/opt/models`, the
  existing `aimee-embedder-models` mount) **and intact**, load it — **no
  download**. "Intact" = the resolved snapshot's tracked files (per
  `huggingface_hub.snapshot_download` for the pinned revision) **all pass the
  hub's LFS SHA verification**; the check is repo-agnostic (the same rule covers
  `pplx-embed-v1-4b`, `pplx-embed-v1-0.6b`, and the reranker). A truncated/corrupt
  or sha-mismatched cache (killed mid-fetch, fs damage) is **re-fetched** with a
  clear "cache incomplete, re-downloading" log — never a cryptic load error.
- Else fetch it once (sentence-transformers / `hf download`) into the volume,
  then load.

`/opt/models` is a **persistent volume dedicated to the embedder** (the existing
`aimee-embedder-models` mount), so a `restart`, `recreate`, or image update
reuses the cached weights — only `down -v` forces a re-download. It is **not
shared with the curator**: the curator gets its own model volume, so the two
containers never race to write the same cache on a cold start.

- **Pin by default.** `EMBEDDER_MODEL` / `RERANKER_MODEL` default to a **specific
  HF revision (commit sha)**, not a moving tag — so "is it present" is exact and
  an upstream re-tag can't silently swap weights. A pin change is a visible,
  logged re-download, not an accident.
- `EMBEDDER_MODEL` (default `pplx-embed-v1-4b@<commit-sha>`) and `RERANKER_MODEL`
  (default `ettin-reranker-1b-v1@<commit-sha>`) are **env vars**, already the build-args
  today — now read at runtime. Switch to the light tier by setting
  `EMBEDDER_MODEL=…-0.6b` + `RERANKER_MODEL=…-400m-v1`; no image swap, no rebuild.
- **First-boot serving policy.** While the model downloads (minutes for the 4b),
  the embedder `/health` is "loading" and the kb's `/v1/health` reports the
  embed tier **degraded** (not failed); the kb **refuses vector search** (clear
  "embedder warming up" error) rather than serve against a not-yet-loaded model —
  no 0-vector fallback. It flips to ready once the model is loaded.
- `HF_TOKEN` covers gated repos. Note the **default is gated**: Gemma-family and
  some others require accepting a license on HF, so a bare first boot fails with
  a 401/403 until `HF_TOKEN` (with the license accepted) is set — surface that as
  a clear "model is gated; set HF_TOKEN or pick an ungated model" error, not a
  silent hang. Air-gapped deploys pre-populate the volume.
- **Model source is operator-trusted.** `EMBEDDER_MODEL` runs arbitrary fetched
  weights (and `trust_remote_code` for some); treat it like any image the
  operator chooses to run. Document it; optionally allow an org/repo allowlist.
- **One published image.** `aimee-embedder-0.6b` is retired — the tier is an env
  var, not a separate SKU.

### 2. Auto-dimension handshake (the KB derives `embedding_dim`)

The embedder `/health` returns `{"model":…, "dim":N}` — but **only after the
model is loaded** (a hard requirement on the embedder: during load it reports
"loading" / 503, never a placeholder dim, so the KB never sizes the schema to a
not-yet-loaded model). The dimension has a strict **precedence**, enforced in
code, not prose:

> **operator pin (`embedding_dim` / `AIMEE_EMBEDDING_DIM`) > recorded
> `schema_embedding_dim` > embedder `/health` probe.**

- **Recorded dim is the source of truth once set, and authoritative for the
  process lifetime.** The first schema apply writes `schema_embedding_dim` as a
  first-class row (a `kb_meta` table) **in the same transaction** as the `halfvec`
  DDL. Every startup reads that row first; the embedder probe is consulted **only
  when no record exists** (fresh DB). Once recorded, a later embedder probe
  reporting a different dim does **not** hot-re-derive or hot-fail — the recorded
  dim stands; a real switch goes through the explicit reset path below.
- **Fresh DB:** no recorded dim and no pin → the KB waits for the embedder healthy
  *with a loaded model*, polls `/health` until `dim` is present and stable, then
  — **holding a Postgres advisory lock** (`pg_try_advisory_lock(<const>)`) so two
  racing KB starts/replicas can't both bootstrap — `db2_set_embedding_dim(dim)`,
  creates the `halfvec` columns, and records `schema_embedding_dim`, all in one
  txn. Non-lock-holders wait, then read the now-recorded row. A configurable wait
  budget (default ~300s) fails fast with a clear error if the embedder never loads.
- **Pin is the operator's backstop**, especially on a fresh DB: if set, it wins
  over the probe and the KB **fails fast** when the embedder reports a different
  dim (catches a misconfigured/stale embedder before it sizes the schema wrong).
- **Populated DB, dim matches:** no-op.
- **Populated DB, dim differs** (operator changed `EMBEDDER_MODEL`): refuse and
  instruct by default — **never** silently serve a 2560 embedder against a 1024
  store. **The re-embed is NOT automatic via the existing backfill:**
  `kb_doc_embed_backfill` only embeds chunks with *no* `kb_embeddings` row — a
  wrong-dimension vector is *present*, so the backfill skips it (this would
  silently strand the corpus). The dim switch is therefore an explicit operation:
  **drop** the embedding rows + their `halfvec` columns, recreate the columns at
  the new dim + record the new `schema_embedding_dim`, restart the kb — *now* the
  chunks have no vector, so the **indexer-side doc-embed pass (`kb_doc_embed_backfill`,
  the kb ingest drain — not the curator)** re-embeds them at the new dim. Chunk
  text and `file_contents` are untouched. This is exactly the sequence proven on
  the .254 4b migration.

  **It is a maintenance operation, not a hot migration, and is double-gated.**
  `kb_reembed_on_dim_change` defaults **off (refuse-and-instruct)**; turning it on
  is not enough — the reset also requires a fresh explicit confirmation
  (`aimee kb reembed --confirm`, or a one-shot confirm token matching the current
  state) so a stale `=on` in config can't silently nuke a corpus on the next
  model change. While it runs, the kb reports `/v1/health = maintenance` (not
  `ready`), search returns "re-embedding, retry shortly" (no partial/empty
  results), and it records pre-drop vs post-backfill chunk counts — divergence
  raises `degraded` rather than passing silently.

## What this removes

- The second embedder image (`aimee-embedder-0.6b`) and its CI matrix entry.
- The multi-GB model layers in the published image (and the slow release builds).
- The class of "search returns nothing because the dim and model drifted" bugs.
- The need to keep `embedding_dim` manually in sync with `EMBEDDER_MODEL`.

## Migration

- Existing baked deploys keep working (the cached model loads the same way); the
  volume just becomes the source of the weights instead of the image layer.
- `.254` is already at 4b/2560 with a populated, consistent store — no change
  unless the model is switched, which then takes the guided re-embed path above.
- Keep `embedding_dim` honored as a pin for anyone who set it.

## Risks / open questions

- **First-boot network + download** (one-time, cached). Mitigate with a long
  health `startPeriod`, the persistent volume, and air-gap pre-population.
- **Startup ordering:** the KB must wait for the embedder `/health` before
  sizing the schema on a fresh DB. The split stack already has the KB depend on
  the embedder being healthy; formalize the dim probe there.
- **Pinned reproducibility:** a baked image is byte-identical; a runtime fetch
  can drift if upstream re-tags. Pin model revisions (HF commit sha) in the
  default to keep it reproducible.
- Should `embedding_dim` mismatch on a populated DB **auto-reset** (drop +
  re-embed) or **refuse and instruct**? Default to refuse-and-instruct; make
  auto-reset opt-in.
