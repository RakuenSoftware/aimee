# Proposal: make the eval temp store openable, or retire it

- **State:** PENDING — tech debt writeup, no design commitment. Records why the two
  standalone eval drivers were deleted and what has to be decided before any
  scratch-store harness can run again. Nothing here blocks shipping; the live
  benchmark path (`memory.benchmark` through `aimee-server`) is unaffected and
  works.

## What was removed, and why

`../aimee-negation-eval` and `../aimee-blast-radius-eval`, their two `main()`
files, and the now-orphaned `tests/support/agent_runtime_eval_stub.c`.

Neither binary had built for some time, and neither could run once it did. Four
separate defects, each hiding the next — the order matters, because it is why this
went unnoticed:

1. **Unbuildable.** Both targets required
   `$(OBJDIR)/kb/modules/agent_eval/*.o`. `modules/agent_eval/` does not exist —
   those sources are `modules/benchmarks/`. `make ../aimee-negation-eval` died
   with `No rule to make target`. *(Fixed, then removed with the targets.)*
2. **API drift, masked by (1).** `mem_negation_eval_main.c` called the
   three-argument `mem_eval_load_corpus`, which had gained an explicit
   embedder-command parameter. Invisible while the link never got that far.
   *(Fixed before removal.)*
3. **No embedder resolution.** `config_embedder_command` reads `EMBEDDER_URL`,
   then the `embedder_command` config field. It does **not** read
   `embedder_model` — that is the wizard's field. The kb entrypoint exports
   `EMBEDDER_URL` into the processes it spawns, so a separately-exec'd binary sees
   nothing and gets the empty string, i.e. "no embedder configured". Anyone
   running one of these by hand has to set `EMBEDDER_URL` explicitly.
4. **The scratch store cannot open.** The blocker below.

## The actual blocker

`db2_eval_open_temp_store_pg` (`src/db2/db2_init.c`) creates a private
`aimee_eval_<pid>_<seq>` schema, sets `search_path` to it, and applies the full
schema there. The design comment is explicit that this is a safety property: eval
tables **shadow** same-named production tables, so pointing the harness at a live
database cannot corrupt it, with `public` left on the path only so pgvector and
pg_trgm types resolve.

`src/db2/schema.sql` addresses objects as `public.<name>` in roughly **950
places**. Applied into a shadow schema on a fresh database, tables are created in
`aimee_eval_*` while those references still resolve to `public`, which is empty.

Two concrete failures, in order:

```
ERROR: relation "public.kb_admin_grant" does not exist       -- kb_principal_is_admin()
ERROR: relation "public.kb_vault_rewrap_operation" does not exist
       CONTEXT: compilation of PL/pgSQL function "inline_code_block"
```

`SET check_function_bodies = off` (this branch) clears the first — the mechanism
`pg_dump`/`pg_restore` use to load functions whose dependencies are absent. It
does not clear the second: PL/pgSQL compiles a `DO` block body before its
`to_regclass` guard can skip it. There are 4 such blocks and 5 guarded
references.

**The shadow-schema design and the schema's addressing convention are mutually
exclusive.** This is not a bug in either one; it is an unreconciled boundary.

## What must NOT be done

Dropping the `public.` qualifiers to make the harness work. At least one is
load-bearing for security: `kb_principal_is_admin()` is the admin gate behind the
tenant-write RLS policies. Unqualified, it resolves through `search_path`, which
is precisely the escalation the qualifier prevents — an attacker able to prepend a
schema could supply their own `kb_admin_grant`. Weakening a production security
boundary to make a dev-only harness run is the wrong trade in any direction.

## Options, when someone picks this up

1. **Apply into `public` of the disposable database; drop the shadow schema.**
   Smallest change, works immediately, matches the schema's assumptions.
   `AIMEE_DB2_EVAL_URL` already documents "a DISPOSABLE Postgres; never the
   production DSN", so the contract never truly depended on shadowing — it was
   defence in depth on top of a rule that already existed. Cost: that defence is
   gone, so it should be replaced with an explicit refusal when the target looks
   like a production database, and ideally a refusal when the target is non-empty.
2. **Make the schema relocatable** — ~950 sites in a security-sensitive file.
   Correct in principle, disproportionate in practice, and it would put the admin
   gate's resolution back on `search_path` unless every call site were audited.
3. **Retire the scratch-store concept.** The live suites
   (`code-graph-fusion`, `memory`, `corpus`, `memory-retrieval`, `live`) run
   against real storage through `memory.benchmark` and need none of this. If no
   harness needs an isolated corpus load, `mem_eval_open_temp_db` and
   `db2_eval_open_temp_store` can go with it.

Option 1 is the cheapest path back to a running harness. Option 3 is worth
considering first, since the machinery currently has one consumer left.

## What was kept

`mem_eval_open_temp_db` and `db2_eval_open_temp_store` remain: they still back
`src/tests/test_memory_retrieval_eval.c` and `mem_eval_load_corpus`. The
`check_function_bodies` fix stays, as it is correct and removes one of the two
failures regardless of which option is chosen.

## Not affected

- `benchmarks/smoke.sh` → `run-llm.sh`, run per-PR by
  `.github/workflows/bench-smoke.yml`. Never touched this path.
- `memory.benchmark` through `aimee-server`. Verified working against a live
  deployment: 19 labelled queries, 0 errors, MRR/NDCG@10/Recall@10 all 1.0,
  latency p50 171.9 ms / p99 339.4 ms. (Those metrics are a plumbing signal, not
  a quality one — the corpus is durable memories used as their own answer key —
  and the latency is end-to-end RPC, not the in-process retrieval latency any
  `<20 ms p99` target refers to.)
- `benchmarks/memory/poison_gate.py`. Runs standalone, gate passes.
