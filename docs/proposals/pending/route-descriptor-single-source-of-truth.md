# Proposal: A single source of truth for `/v1` routes

- **State:** PENDING — design only, no code in this PR. Proposes collapsing the
  parallel hand-maintained `/v1` route/dispatch tables into one descriptor that
  the others are generated from. Builds on the already-landed declarative route
  table (`server_http_routes` `g_v1_routes[]`, WP0.2 / #2531) and the two drift
  guards that exist *because* the tables fall out of sync by hand
  (`scripts/check-cli-v1-routes.py`, `scripts/check-api-conformance-server.py`).
  It does not re-propose the op-parity buildout
  (`docs/v1-op-parity-buildout.md`); it removes the per-route hand-sync that
  buildout left in place.
- **Author:** JBailes
- **Date:** 2026-07-19
- **Charter roles:** Reason (design the descriptor schema and the generation
  boundary), Execute (codegen step + table-by-table migration behind the
  existing drift gates), Review (the two drift-checkers become the generator's
  self-test rather than a manual-sync tripwire), Persist (the descriptor is the
  durable record of the wire contract).

## Thesis

Adding or changing **one** `/v1` route today requires hand-editing the same fact
— a method name, its HTTP verb+path, timeout, output shape — into **eight to nine
parallel tables across six files plus an OpenAPI spec**, all keyed on the same
magic method string. The project already knows this hurts: it built **two** CI
drift-checkers whose entire job is to catch the tables falling out of sync, and
`docs/v1-op-parity-buildout.md` documents the per-route ritual ("1. One table
row. 2. One OpenAPI path…"). This proposal makes one table the source of truth
and *generates* the rest, turning "9 edits + 2 tripwires" into "1 row."

## The current sync surface (verified against `testing` @ this branch's base)

One logical route is hand-registered in each of these:

| # | Table / mechanism | Location | Size | Keyed on |
|---|---|---|---|---|
| 1 | `rpc_routes[]` (cmd/subcmd → method, timeout, extract field) | `src/cli_v1_routes.c:140` | **~197 rows** | cmd+subcmd |
| 2 | `marshal_request()` `strcmp` chain (method → marshaler) | `src/cli_v1_routes_b.c:1182` | **128 branches** | method |
| 3 | per-method `marshal_*()` functions | `src/cli_v1_routes*.c` | dozens | method |
| 4 | `pt_print_table[]` (method → human formatter) | `src/cli_v1_routes_d.c:27` | ~122 rows | method |
| 5 | `CLI_V1_GEN_ROUTES[]` (method → verb+path) — **already generated** | `src/cli_v1_routes_d.c:552` | ~205 rows | method |
| 6 | `g_v1_routes[]` (verb+path → method → handler) | `src/server/server_http_routes.c:1351` | ~323 rows | path+method |
| 7 | `server_dispatch_table[]` (method → handler fn) | `src/server/server.c:1378` | ~230 rows | method |
| 8 | OpenAPI contract | `api/openapi-server-v1.yaml` | ~301 paths | path |
| 9 | Top-level command routing + help text | `src/cli_main.c` | — | cmd |

The two drift-guards are the load-bearing evidence that this is a real, recurring
cost, not a hypothetical: `scripts/check-cli-v1-routes.py` and
`scripts/check-api-conformance-server.py` exist to fail CI when these tables
disagree.

Crucially, **#5 is already the pattern this proposal generalizes.**
`CLI_V1_GEN_ROUTES[]` is not hand-maintained: it lives between
`@@GEN-CLI-V1-ROUTES BEGIN/END` markers (`src/cli_v1_routes_d.c:536–767`),
emitted from `g_v1_routes[]` (#6) by `scripts/gen-cli-v1-routes.py`, and
`check-cli-v1-routes.py` *regenerates the block and diffs it against what is
committed* — it is already a generator self-test, not a manual tripwire. So one
derived table (#5) is generated from one source (#6) today; this proposal is the
same move applied to the whole set, with the source raised to a dedicated
descriptor instead of #6.

## Proposed design

### 1. One descriptor per route

Define a single array of route descriptors — a `.def`/X-macro list or a small
declarative table compiled at build time — carrying every fact the nine tables
today each hold a slice of:

```
{ cmd, subcmd, method, verb, path, timeout, extract_field,
  marshaler, printer, capability, body_shape }
```

### 2. Generate, don't hand-sync

A build-time generator — the same shape as the **already-working**
`scripts/gen-cli-v1-routes.py`, which today emits `CLI_V1_GEN_ROUTES[]` (#5)
from `g_v1_routes[]` (#6) between in-file markers — emits from that descriptor:

- the CLI `rpc_routes[]` (#1) and the `marshal_request` dispatch (#2, as a
  keyed lookup rather than a 128-way `strcmp` chain),
- the printer dispatch `pt_print_table[]` (#4) and `CLI_V1_GEN_ROUTES[]` (#5),
- the server `g_v1_routes[]` (#6) and `server_dispatch_table[]` (#7),
- the OpenAPI paths (#8), reconciled with the existing generator.

The per-method `marshal_*` / `print_*` *function bodies* (#3) stay hand-written —
they encode genuine per-method logic — but they are *referenced* by the
descriptor, so a method with no special marshaling defaults to `marshal_no_args`
and needs no row at all.

### 3. The drift-checkers become the generator's self-test

`check-cli-v1-routes.py` **already** works this way for #5 (regenerate-and-diff
against the committed block); the proposal generalizes that stance so
`check-api-conformance-server.py` and the other tables' checks likewise verify
generated-output-matches-descriptor — protecting the generator, not policing a
human's manual sync.

## Incremental migration path (each step ships green behind the existing gates)

This is deliberately **not** a big-bang rewrite. The existing drift-checkers make
a table-by-table migration safe: at every step the generated table must be
byte-identical to the hand-maintained one it replaces, and the checker proves it.

1. **Descriptor + generator, generating nothing yet.** Land the descriptor
   populated from the current tables and a generator that emits each table to a
   `*.gen` file. A new check asserts every `*.gen` equals the checked-in
   hand-written table — zero behavior change, pure proof the descriptor is
   faithful.
2. **Repoint the existing generator + generate the other CLI tables.** #5 is
   already generated from #6 by `gen-cli-v1-routes.py`; repoint it to read the
   descriptor instead, and bring #4 (`pt_print_table[]`) under the same
   generator. Lowest risk (CLI-only, and #5's machinery already exists).
3. **Flip the CLI dispatch** (#1/#2), collapsing the 128-branch `strcmp` into a
   generated keyed lookup.
4. **Flip the server tables** (#6/#7) — the highest blast radius, done last,
   with `server-api-conformance-check` + `v1-method-coverage-check` as the net.
   Note the data-flow inversion: today #6 is the *source* that #5 is generated
   from, so this step is where #6 stops being the root and becomes a generated
   consumer of the descriptor; sequence it so #5's generator has already been
   repointed off #6 (step 2) before #6 itself is flipped.
5. **Reconcile OpenAPI generation** (#8) against the descriptor. This one
   cannot be a clean emit: the OpenAPI paths carry per-path request/response
   *schemas* the compact descriptor row does not hold. The realistic scope here
   is generating the path/verb/method skeleton from the descriptor and keeping
   the schema bodies in their own source, reconciled by the existing conformance
   check — not folding OpenAPI wholesale into the descriptor.

Any step can stop and ship; the descriptor and the hand tables coexist until
each is flipped.

## Risks and non-goals

- **Blast radius.** #6/#7 are the hot request path. Mitigation: they migrate
  last, gated by the existing conformance checks, and the `*.gen`-equals-checked-in
  assertion means a mismatch fails CI before merge, never at runtime.
- **Generator-in-the-build complexity.** Adds a codegen step to a codebase that
  already has several (`docs-gen`, `gen-config-surface`, `gen-reference-docs`),
  so the pattern and its CI gate are established, not novel.
- **Not a wire-contract change.** The generated tables must reproduce today's
  routes exactly; this proposal changes *how* the tables are maintained, never
  *what* they serve. No client sees a different response.
- **Non-goal:** the per-method `marshal_*`/`print_*` bodies and `cli_main.c`
  help text are out of scope beyond being referenced/derived; this is about the
  *registration* surface, not the per-method logic.

## Effort

Multi-day, multi-PR. Step 1 (descriptor + faithful-generation proof) is the
bulk of the design risk and is independently valuable even if later steps are
deferred: it makes the nine tables' agreement machine-checked against one
source. Steps 2–5 are mechanical flips, each small and independently
reviewable/revertible.
