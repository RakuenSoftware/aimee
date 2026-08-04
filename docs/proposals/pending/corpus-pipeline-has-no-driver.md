# The corpus pipeline has no driver

**Status:** pending — found during overnight validation of the bekko + E2B deployment
on CT 302, 2026-08-02. Deliberately NOT fixed in that pass: every candidate fix
changes what background work a deployment performs, and that is a decision to take
awake.

## What happens

`aimee kb docs push <file>` reports success and the document is never processed.

```
$ aimee kb docs push /tmp/kbdocs/note.md
Docs push complete: 1 uploaded, 0 skipped, 0 failed (1 total).

$ aimee kb status
chunks:        0
vector points: 0 kb, 17 memory
```

The row exists and is waiting:

```sql
SELECT doc_id, stage, stage_status, attempts, lease_owner FROM corpus_processing_jobs;
 doc_id |  stage   | stage_status | attempts | lease_owner
--------+----------+--------------+----------+-------------
      1 | ingested | pending      |        0 |
```

It stays there indefinitely.

## Why

`db2_corpus_pipeline_drain()` has exactly two callers:

- `POST /v1/corpus/pipeline/drain` (`src/kb/http/kb_http.c`)
- `aimee kb pipeline drain` (`src/cmd_kb.c`), which is a client of that route

Nothing calls it on a timer. The curator drain thread
(`kb_curator_drain.c`, `DRAIN_POLL_SECS 5`) looks like the natural driver but is a
different queue: it calls `kb_curator_queue_docs_all_projects()`, which feeds
`kb_async_jobs`, not the 14-stage `corpus_processing_jobs` conductor in
`db2/corpus_jobs.c`.

The pipeline itself is fine. Driven by hand it completed all 14 stages
(`ingested` → `complete`) with zero failures and wrote 14 `corpus_stage_events`:

```
$ curl -X POST .../v1/corpus/pipeline/drain -d '{"limit":50}'
{"state":"idle","processed":14,"pending":0,"failed":0,"done":1,
 "stages":[{"stage":"complete","status":"complete","count":1}]}
```

## The second half: the operator cannot reach it either

`aimee kb pipeline` is a shipped subcommand of `kb_subcmds[]`, but in a managed
deployment the thin client dispatches through aimee-server's `/v1` table, and the
corpus pipeline routes live on the kb rather than being surfaced by the server:

```
$ aimee kb pipeline status
aimee: 'pipeline' is not a subcommand of 'kb'; try: search, build, update, docs push, ...
```

So the only way to advance a pushed document today is to `docker exec` into the kb
container and curl its loopback port. `check-kb-intelligence-surfaced` reports
"9 routes surfaced via aimee-server, 0 kb-direct", so whether these two routes are
*meant* to be surfaced is a real question, not an oversight to patch blindly.

## Why this was not fixed in the same pass

The obvious fix — call the drain from the curator drain thread — starts LLM-backed
stages (`summarized`, `questions_generated`, `entities_extracted`,
`claims_extracted`, `relationships_mapped`, `conflicts_detected`, `gaps_detected`)
automatically on every deployment. On the bundled CPU sidecar a single extraction
already costs 80-170s, and the sidecar serves one slot. Turning that on unreviewed,
overnight, could saturate synthesis on every existing install at upgrade.

## Options, for a decision rather than a recommendation

1. **Drive it from the curator drain thread**, gated by a config key that defaults
   off, so the behaviour is opt-in and the load is chosen. Most useful, most blast
   radius.
2. **Surface the two routes through aimee-server** so `aimee kb pipeline
   status|drain` works as shipped, and leave the driving manual. Smallest change;
   makes the existing CLI honest.
3. **Decide `docs push` should not seed the pipeline at all** and have it say so, if
   the corpus conductor is meant for a path that no longer exists.

Options 1 and 2 are independent and 2 is worth doing regardless: a subcommand that
cannot be invoked is a defect on its own.

## Not in scope, but adjacent

`kb docs push` populates `docs` and `document_sections`; the searchable corpus
(`kb_documents`, `kb_embeddings`) is populated by `aimee kb build --path P --project
N`, which works correctly — verified on the same deployment: 2 files, 2 chunks,
4 embeddings at dim 384, and semantic search ranked the then-present
src/reconcile.py (0.5164) over
`README.md` (0.3170) for "matching payments against statements", a query sharing no
terms with either. Anyone reading "chunks: 0" after a `docs push` is likely to
conclude the embedder is broken when it is not.
