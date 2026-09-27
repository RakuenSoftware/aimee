# Proposal-only memory hygiene

Use `aimee memory hygiene --scope project:example --dry-run --json` to inspect a
bounded scope. Omit `--dry-run` to queue findings for review. Normal admission
requires authenticated host context and memory-write capability. There is no
apply switch. Review accepts or rejects a finding; canonical edits still require
the ordinary exact-version mutation/review interface.

The Go owner inspects at most 128 retained records and 32,768 content bytes per
page. Defaults are 64 records and 16,384 bytes. Current, authorized content is
compared for exact duplicates using a scoped index, including matches on other
pages. A cluster contains at most 16 versions; larger clusters are explicitly
partial. Metadata detectors report expired validity, unavailable correction
successors, invalid derived-input proofs, observations without visible memory
links, stored unresolved conflicts, and low-confidence records with at least
eight visible incoming links. These are candidates, not semantic judgments.
Hidden or ineligible peers never become samples or cluster counts. Missing and
unauthorized successors are deliberately indistinguishable.

Delivered-exposure detection has no adapter and is labeled unavailable. Model
assistance is disabled: no model calls, tokens, or billing costs occur on this
path. Stored conflicts are not new model-authored contradictions. The report's
`detectors` map defines coverage; a complete page is not a clean bill of health
for unavailable detectors. A lack of recent use never authorizes deletion.

`--cursor TOKEN` resumes a partial page. Owner, scope, collection generation and
policy bind the token. Canonical changes require restarting the scan. Oversized
content or cluster limits may leave partial coverage without further progress;
increase the applicable budget where possible or inspect the reported scope.
The database query has a two-second deadline and admission has a five-second
owner deadline. Failed work returns unavailable, never an empty clean report.

Normal pages record an idempotent run and claim an existing `kb_async_jobs` job
within the same transaction as proposal creation. `job_telemetry_written`
identifies these noncanonical writes. Dry runs write neither jobs nor proposals.
Identical findings reuse their proposal even after rejection or expiry. Proposals
bind exact record versions, remain immutable, and cannot be accepted after their
inputs change. The detector role has SELECT access and narrow queue functions;
it has no canonical mutation or proposal-review privileges.

## Optional scheduling

Scheduling is disabled by default. An operator can explicitly invoke this bounded
tick from an existing timer, using the CLI paired to the intended deployment:

```sh
python3 scripts/memory_hygiene_schedule.py \
  --cli /absolute/path/to/aimee --scope project:example \
  --state /operator-owned/path/hygiene-example.json \
  --max-pages 4 --max-seconds 30
```

The tick uses a local nonblocking lock, saves a content-free cursor atomically,
and preserves it on failure. It reports stalled partial coverage. `--restart`
explicitly starts a new snapshot after a collection change. It installs no
service, schedules no model work, and does not invoke broad maintenance. Remove
the timer to stop scheduling; retain learning proposal history.

## Expired task projections

`aimee task projection cleanup_expired --session-id SESSION --task-id TASK
--expected-revision REVISION --json` asks the private session owner to clean one
expired disposable projection. It can name an old task after a session switch.
Principal/session ownership, revision, TTL retention and the session's workflow
lease are checked under transaction locks. A live TTL or workflow lease blocks
cleanup. Only derived items, dependency text and pending submission text are
cleared; immutable admission digests and execution receipts survive. This route
never sweeps other sessions or modifies canonical memory. It is separate from
shared-memory hygiene and is not automatically scheduled by the shared tick.
