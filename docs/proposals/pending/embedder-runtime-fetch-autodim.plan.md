# Implementation plan — embedder auto-dimension §2a: recorded-dim precedence

Plan for [embedder-runtime-fetch-autodim.md](embedder-runtime-fetch-autodim.md),
**scoped to §2a only**: make the recorded `schema_embedding_dim` an actual
*source* of the runtime embedding dimension, under the proposal's precedence
**operator-pin > recorded > probe**. Grounded on `origin/testing`.

## Why this slice (and why now)

`#337` made `kb_meta.schema_embedding_dim` a **guard**: `db_apply_schema_postgres`
calls `db2_embedding_dim_record_or_check()`, which records the dim on first apply
and *refuses* a later mismatch. But nothing ever **reads that row back as the
dimension to use** — there is no `db2_embedding_dim_get()`, and the runtime dim is
fixed entirely from the operator pin:

- `cmd_core.c:51` and `kb/kb_main.c:544` both call
  `db2_set_embedding_dim(config_resolve_embedding_dim(cfg))` **before** `db2_init()`.
- `config_resolve_embedding_dim()` (`config_database.c:67`) returns
  `AIMEE_EMBEDDING_DIM` (env) ▸ else `cfg->embedding_dim` ▸ else `0` → which
  `db2_embedding_dim()` reports as the **1024 default**.

Consequence: on a **populated** DB where the operator did *not* pin a dim, the
recorded dim is ignored. If the recorded corpus is 2560 but the unpinned default
is 1024, today's guard **refuses the apply** (mismatch) — the operator must
hand-set the pin to match what the DB already knows. That is the exact
"keep `embedding_dim` manually in sync" failure the proposal removes (§"What this
removes", §2 precedence). The recorded value should simply **win over the
default** when the operator hasn't pinned.

## Scope (and explicit non-scope)

**In:** the *recorded* leg of the precedence, plus the pure selector that encodes
the full precedence so §2b can drop the probe in with a one-line change.

**Out (separate, live-stack slices — named here so this is a reserved contract,
not forgotten work):**
- **§2b** — fresh-DB auto-derive from the embedder `/health` **probe** under
  `pg_try_advisory_lock`, with a wait budget. Needs a live embedder; this slice
  leaves the `probed` input wired but always `0` (reserved, exactly as P0 reserved
  `ING_XF_*`).
- **§2c** — the **destructive** double-gated auto-reembed
  (`kb_reembed_on_dim_change` + `--confirm` + `/health=maintenance`). Out of scope;
  this slice never drops or rewrites a vector.

This slice changes behavior **only** in the populated-DB, operator-did-not-pin,
recorded-differs-from-default case — which is a *refused/wrong* outcome today.
Pinned behavior and fresh-DB behavior are byte-identical to today.

## Design

> **Plan-review R1 (2026-06-21, mistral APPROVE / minimax CHANGES) folded in.**
> Single precedence function (no separate dead-code selector); precise
> pin-detection; `g_embed_dim_pinned` reset on shutdown; all three call sites
> enumerated; structured-logger WARN; bounded reader. The reserved-`probed`
> three-arg selector is **dropped** (YAGNI) — `probed` returns in §2b.

### 1. One precedence function — pure, exported, unit-tested directly (no DB)

The §2a precedence collapses to a single rule. Ship **one** function (kills the
plan-R1 "selector is dead code, db2_init uses a different helper" blocker — db2_init
calls *this* function, and the unit test calls it too):

```c
/* db2/lifecycle.h + db2_init.c — the effective embedding dim under §2a precedence
 * (pin > recorded > configured-default). `pinned`: operator gave a positive pin.
 * `configured`: the dim already set pre-init (the pin when pinned, else the
 * 1024-or-cfg default). `recorded`: kb_meta.schema_embedding_dim, or <=0 if absent.
 * Pure; no globals, no I/O — the one place §2a's precedence lives. */
int db2_effective_dim(int pinned, int configured, int recorded);
```

`if (pinned) return configured;` (pin authoritative) `else if (recorded > 0)
return recorded;` (recorded wins over default) `else return configured;` (default).

### 2. Recorded-dim reader (one-row read; testable against the sqlite shim)

```c
/* db2/db_schema.h / .c — read the recorded schema dim. Returns the recorded dim
 * when in range (1..EMBED_MAX_DIM), else 0 — the "absent" signal db2_effective_dim
 * treats as not-present. Out-of-range/garbage/overflow → 0 (defends strtol against
 * operator typos; per plan-R1 SUGG 5). Read-only; never writes. Uses aimee_pg_*
 * so it works against Postgres and the sqlite test shim. */
int db2_embedding_dim_get(void *conn);
```

`SELECT value FROM kb_meta WHERE key = 'schema_embedding_dim'` → `strtol` → return
the value iff `1 <= v <= EMBED_MAX_DIM`, else 0. (The corrupt/non-numeric *write/
check* path keeps its loud refusal in `record_or_check`; the *reader* stays quiet
and returns 0 so the caller falls through to its default — a missing/garbage row
must not crash a read.)

### 3. "Operator pinned?" signal — defined precisely (plan-R1 BLOCKER 2)

```c
/* config_database.h / .c — 1 iff the operator pinned a positive dim. Defined as
 * config_resolve_embedding_dim(cfg) > 0 so "pinned" is exactly consistent with the
 * value db2_set_embedding_dim received: AIMEE_EMBEDDING_DIM="0"/non-numeric/empty
 * is NOT a pin (config_resolve already ignores it -> 0), nor is an unset cfg dim. */
int config_embedding_dim_is_pinned(const config_t *cfg);
```

The db2 layer stays config-free; the caller passes the bool via a setter mirroring
`db2_set_embedding_dim`:

```c
void db2_set_embedding_dim_pinned(int pinned);   /* db2/lifecycle.h + db2_init.c */
```

`g_embed_dim_pinned` is a file-static in `db2_init.c` (default 0). **Reset to 0 in
`db2_shutdown()`** (db2_init.c:619) so a daemon reopen / a later test cannot inherit
a previous run's pinned state (plan-R1 BLOCKER 3).

### 4. Wire the recorded leg at the one post-connect boundary (no pre-init change)

Pre-init ordering is **untouched**: each caller still
`db2_set_embedding_dim(config_resolve_embedding_dim(cfg))`; it additionally calls
`db2_set_embedding_dim_pinned(...)` beside it (§Files lists all three sites). Inside
`db2_init()`, after the connection opens and **before** `db_apply_schema_postgres()`:

```c
int dim = db2_embedding_dim();                              /* pin or default */
int rec = g_embed_dim_pinned ? 0 : db2_embedding_dim_get(conn);
int eff = db2_effective_dim(g_embed_dim_pinned, dim, rec);  /* the one precedence fn */
if (eff != dim) {
    LOG_WARN("db2", "using recorded embedding dim %d (no operator pin; configured "
                    "default was %d)", eff, dim);           /* unmissable; plan-R1 SUGG 4 */
    dim = eff;
    db2_set_embedding_dim(dim);                             /* halfvec cols + all readers agree */
}
if (db_apply_schema_postgres(conn, dim, ...) ...) ...
```

`record_or_check(dim)` inside `db_apply_schema_postgres` then sees `dim == recorded`
(match → no-op) on the populated path, instead of refusing. Fresh DB: `rec == 0`,
`eff == dim` → identical to today. Pinned: `rec` forced to 0, `eff == dim` →
identical to today (mismatch still refused downstream, catching a misconfigured pin).

## Files

- `src/config_database.c` / `src/headers/config_database.h` —
  `config_embedding_dim_is_pinned`.
- `src/db2/db_schema.c` / `src/db2/db_schema.h` — `db2_embedding_dim_get`.
- `src/db2/db2_init.c` / `src/db2/lifecycle.h` — `db2_effective_dim` (pure),
  `db2_set_embedding_dim_pinned`, the `g_embed_dim_pinned` static + its reset in
  `db2_shutdown`, and the recorded-preference block in `db2_init`.
- **All three** `db2_set_embedding_dim(...)` call sites get a companion
  `db2_set_embedding_dim_pinned(...)` (plan-R1 SUGG 2 — the audit found a third):
  `src/cmd_core.c:51` and `src/kb/kb_main.c:544`
  (`config_embedding_dim_is_pinned(cfg)`), and `src/cmd_doctor.c:68`
  (sets the dim from `cfg->embedding_dim`, so pinned = `cfg->embedding_dim > 0`).
- Tests: extend `src/tests/test_embedding_dim.c` (shim) + `src/tests/test_config.c`
  (the env/pin block already there). `db2_effective_dim` is pure → unit-test it
  directly. Preferring to extend existing files avoids a new `Rules.mk` target.

All files stay well under the 2000-line cap.

## Tests (all local, no live stack)

1. **`db2_effective_dim` truth table** (pure, the precedence contract):
   `(pinned=0, cfg=1024, rec=2560) → 2560` (recorded wins over default);
   `(1, 1024, 2560) → 1024` (pin authoritative);
   `(0, 1024, 0) → 1024` (fresh DB, nothing recorded);
   `(0, 2560, 2560) → 2560` (match — and the *caller* must then NOT log / NOT
   re-set, since `eff == dim`; plan-R1 SUGG 3);
   `(0, 1024, -1) → 1024` (non-positive recorded treated as absent).
2. **`config_embedding_dim_is_pinned`** — `AIMEE_EMBEDDING_DIM="2560"` → 1;
   `="0"` → 0; `="garbage"` → 0; unset + `cfg->embedding_dim=1024` → 1; unset +
   `cfg->embedding_dim=0` → 0 (plan-R1 BLOCKER 2 — the env="0"/non-numeric rows are
   the point). Reuses the setenv/unsetenv block already in `test_config.c`.
3. **`db2_embedding_dim_get`** (shim) — no row → 0; recorded 2560 → 2560; empty /
   `garbage` → 0; out-of-range (`5000 > EMBED_MAX_DIM`) → 0 (plan-R1 SUGG 5).
   Mirrors the existing fixture in `test_embedding_dim.c`.
4. **Pinned-state isolation** (plan-R1 BLOCKER 3) — set pinned=0 then exercise a
   path; `db2_shutdown()`; assert a subsequent default read sees pinned reset (a
   pinned-mismatch case run *after* an unpinned case still refuses).
5. **Regression** — existing `test_embedding_dim` mismatch-refusal assertions pass
   unchanged (its direct `record_or_check` calls are unaffected).

## Build / verification

- `make lint` + `make unit-tests` before push (catches the usual generated-surface
  + test-link gaps — see prior sweep notes). Build the real feature set, not just
  defaults.
- **Not verifiable locally (honest deferral):** the live populated-DB-on-Postgres
  override and the §2b probe handshake need a real PG + embedder. Mitigated by
  routing the decision through the shim-tested pure helper (#4) so the *logic* is
  fully covered; the live wiring is a thin call into tested code.

## Risks / rollback

- **Blast radius is the unpinned-populated case only** — today a refused/wrong
  result, so the change can only improve it. Fresh-DB and pinned paths are
  unchanged (asserted by the truth-table + integration tests).
- A recorded row that is **corrupt** makes the reader return 0 → falls through to
  today's behavior (no regression, no crash).
- Rollback is a straight revert (one feature, additive symbols + 3 call-site lines).
- No schema change, no config-file change, no wire/provider dependency.
