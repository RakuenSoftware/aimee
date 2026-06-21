# Implementation plan — deepening sweep (Part B)

Plan for **Part B** of [replayable-verification-and-deepening-sweep.md](replayable-verification-and-deepening-sweep.md)
(Part A — the replay engine + verifier + rubric — shipped to `testing` in #558 + #560).
Grounded in `origin/testing`. Analysis-only: the sweep writes work items + per-area
summaries; **zero source edits**.

**Plan-gate:** roundtabled — architect / security / engineer lenses, all
APPROVE-WITH-CHANGES, no blockers surviving. Folded in: B-core gets worktree
**isolation** (can't run in-place on a shared checkout); the `manual-review`
workflow is **defined + shipped** so the quarantine guarantee is real (not a soft
flag); exclusion keys on the **original seam** (`<file>:<top-decl>`, exact) so a
renamed artifact can't bypass it, with a re-attempt policy; rule-of-three gains
**distribution + independence** predicates; `validate_proposal_path` gets
realpath/repo-root/area-root semantics + a malicious corpus; proposal_md is
**opaque untrusted bytes** (consumer contract + injection test); two-tier
INDEX_UNAVAILABLE (per-area skip vs preflight abort); pinned committer identity +
pre-commit hook; caps with real units/values; typed_facts read-path + CI-exclusion
degrade/precondition.

## Builds on Part A (reuse, don't rebuild)

| Primitive | Where | Use |
|-----------|-------|-----|
| `evidence_replay_with(be, ev, &rec)` | `server/evidence_replay.c` (shipped) | Verify a candidate's `EV_REFS` edge-count claim deterministically → `{count, idkey}`. The sweep is its second consumer. |
| `replay_backend_t` + kb_client backend | `evidence_replay.h`, `server_compute.c` (shipped) | Same kb_client-backed code-index access the verifier uses. |
| `kb_client_index_find_callers(project, symbol, …)` | `server/kb_client_index.c` | The deletion-test edge count (callers of the proposed seam). |
| `kb_client_index_blast_radius(project, file, …)` | `server/kb_client_index.c` | Dependents/dependencies for the seam region (shared-state check). |
| `wfe_work_item_create(workflow, repo, proposal_path, …)` | `workflow/wfe_engine.c:97` (via `POST /v1/dev/submit`, `server_http_routes.inc:1686`) | File each survivor as an autonomous-dev work item (default `build` workflow). |
| `db2_typed_fact_assert(...)` + `TF_ONTOLOGY[]` | `db2/typed_facts.c:28` | The `architecture_settled` exclusion record (WP-B1). |
| `aimee delegate <role> --persona … --via …` | shipped | One short proposer sub-agent per area (the sweep does not run one long call). |
| worktree helpers | `server/workspace*.c`, `git_ops.c` | Off-remote-default-branch worktree + per-area commits (WP-B6). |

**Lessons carried from Part A (bake in):** new code in NEW files (line cap);
`clang-format-19` on C only (NEVER on `.mk`); run `make unit-tests` (not just
`make`) before push — a shared `.o` breaks existing test targets; work in a git
worktree (the shared checkout has concurrent actors).

## Execution context (the key decision)

The sweep needs three server-reachable capabilities: the **code index** (kb_client),
**delegates** (server-side), and **work-item filing** (`/v1/dev/submit`). Two shapes:

- **(A) CLI orchestrator** — `aimee sweep` drives the existing server endpoints
  (`/v1/delegate/run` for proposers, the code-index over kb_client, `/v1/dev/submit`
  to file). Thin, reuses everything, no new long-lived server state. **Recommended for v1.**
- (B) Server-side job — a `handle_dev_sweep` like `handle_delegate_roundtable`.
  More moving parts; defer.

Plan assumes (A). The sweep references no db2 directly (consistent with the
AIMEE_DB2_DISABLED client + the Part-A seam).

## Work packets

### WP-B1 — `architecture_settled` relation + exclusion map
- Add `{"architecture_settled", "code_site", "SCALAR"}` to `TF_ONTOLOGY[]`
  (`db2/typed_facts.c`); subject = canonical `file:symbol-prefix`, object =
  `extracted@<commit>` / `rejected:pass-through`, source = proposal/commit ref.
- Exclusion map (deterministic, not fuzzy). The key is the **original seam, not the
  proposed new module** (engineer): `seam_symbol = "<seam_file>:<top-level-decl-name>"`
  taken from the index, compared by **exact equality** — so a proposer can't dodge
  exclusion by renaming the artifact (`foo_extract.c`) while the seam (`foo.c:helper_x`)
  is already settled. A candidate is excluded if its `seam_symbol` matches (a) an
  open work item, (b) an existing `docs/proposals/*` entry, or (c) an **active**
  `architecture_settled` fact (active-only recall). Reads at sweep start, re-read per
  area (drift detection, WP-B6).
- **Re-attempt policy:** `extracted@<commit>` → permanently excluded (already done).
  `rejected:pass-through` → excluded by default, re-allowed only via a new proposal
  that explicitly cites the prior rejection (so a re-decision is a deliberate human
  act, not a silent re-file).
- **Read-path fallback (architect+security):** if `architecture_settled` recall is
  unavailable from the DB2-disabled client (open question below), the exclusion map
  **degrades to proposals + work-queue only**, logs the gap, and notes it in the PRD —
  never aborts.
- Settled-decision **writes** are implement-side (Part A's loop closure), not the
  analysis-only sweep — the sweep only reads.

### WP-B2 — scope → areas + caps (`server/sweep_scope.{c,h}`)
- Resolve areas from a **configured source-glob allowlist** (default `src/**`,
  `tests/**`) — never from sub-agent output. Partition: top-level source subdirs;
  ≤50 files/area; split a too-big dir by `#include`-cluster; fold <8-file leftovers
  into the nearest neighbour.
- Caps (config-backed, units + starting defaults pinned — engineer): **delegate
  calls** = ≤2/area (proposer + at most one narrower retry; verification is
  in-process via `evidence_replay`, no delegate); ≤40 areas/sweep ⇒ ≤80 calls;
  **wall-clock** = 60s/area, 1800s/sweep hard ceiling; ≤10 filed items/area. The
  "projected budget" is `areas × 2` calls, computed after scope resolution; hard
  abort at 1.5× it (covers retries) or the wall-clock ceiling. Per-area log records
  delegate-call count + wall-clock seconds.

### WP-B3 — per-area candidate proposer + mechanical deletion test (`server/sweep_propose.{c,h}`)
- One short delegate per area (one-retry-with-narrower-brief; mark
  `[!] needs manual look` rather than stalling the sweep). The brief asks for
  duplication-across-call-sites candidates, each as a **structured `review_evidence_t`**
  (`EV_REFS`, target = the proposed seam symbol, claimed caller count) — reusing
  the Part-A evidence shape.
- The **deletion test is mechanical, not prose**: a candidate qualifies under the
  **"rule of three"** — `kb_client_index_find_callers` ≥3 independent callers whose
  only shared state (via `kb_client_index_blast_radius`) is the proposed interface.
  Threshold named + config-tunable (Part A rubric vocabulary).
- **Distribution + independence (engineer):** a raw count over-ranks a private static
  helper called 30× in one file vs. a real cross-subsystem seam. So the rule-of-three
  also requires a **distribution predicate** — ≥2 distinct files (or ≥2 subsystems) —
  and **independent callers** = no common caller within graph distance N (default 2)
  over the same kb_client edge source. A candidate that clears the count but fails
  distribution/independence is demoted to `needs-manual` (worth-exploring), not Strong.
- **Shared-state tolerance (mistral):** the rule-of-three is a heuristic — callers
  legitimately share logging/metrics/context, and blast_radius undercounts indirect
  edges (macros, fn-pointers). Add a config `sweep.shared_state_tolerance` (default 1);
  a candidate whose shared-state count exceeds it is **not auto-strong** — it is
  filed as `needs-manual` (worth-exploring), never silently ranked Strong. The
  limitation is stated in each area PRD.

### WP-B4 — verify + cross-area dedup (`server/sweep_verify.{c,h}`)
- Verify every candidate with **`evidence_replay_with`** (the shipped engine): the
  caller-count claim must reproduce; `CONTRADICTED`/`VACUOUS` → dropped (recorded
  in the PRD's "rejected at verification" list). `INDEX_UNAVAILABLE` handling is
  **two-tier** (mistral): a **per-area/transient** miss → skip the area, mark it
  `[!] needs manual look`, continue (don't lose already-processed areas); only a
  **whole-index-down** preflight (project_count==0 at sweep start) aborts up front
  with a clear message (a sweep with no index can verify nothing).
- **Known limitation (engineer):** with a populated index, `find_callers`==0 is
  graded `CONTRADICTED` (drop) — it does not distinguish "symbol genuinely has no
  callers" from "the symbol's file was never scanned." The index is treated as
  authoritative; unscanned-coverage is surfaced separately (a coverage warning in
  the PRD), not papered over by reclassifying drops as skips. Adding a NOT_FOUND
  status to the engine is out of Part B scope.
- Cross-area dedup on `sha256(reduced_record)` (the engine's `idkey`), computed by
  the verifier, not the proposer — the same duplicated rule from two areas merges
  into one work item.

### WP-B5 — vertical-slice work-item filing (`server/sweep_file.{c,h}`)
- Each survivor → a **vertical-slice** proposal_md (the deep module + every call
  site repointed + tests at the new interface + old copies deleted) filed via
  `wfe_work_item_create`. **Filed to a non-auto-merging workflow, NOT `build`**
  (mistral-security): the sweep's output is untrusted, so it lands on a
  `manual-review` workflow that requires a human to promote it to `build`.
- **The `manual-review` workflow must actually exist (engineer-blocker):**
  `wfe_work_item_create(workflow, …)` takes a workflow id — if `manual-review` is
  undefined it errors or silently creates one and the quarantine guarantee
  evaporates. So this PR **ships `$AIMEE_HOME/workflows/manual-review.yaml`** (seeded
  like the existing `build.yaml`) with a `requires_human_promote: true` gate before
  any implement/build step, and a precondition check that refuses to file if the
  workflow is absent. Cite it next to the existing build.yaml seed.
- **proposal_md body is opaque/untrusted (engineer):** `validate_proposal_path`
  covers paths, not the body. State the consumer contract — the proposal markdown is
  **stored and passed as opaque bytes**: no shell eval, no template render, no
  Markdown→HTML pass-through, no `patch`-applying of embedded diffs without the human
  gate. A unit test injects `` `id` ``, `$(touch /tmp/x)`, and a backtick-laden
  `tests/` ref into the body and asserts it is filed byte-unchanged onto
  `manual-review` (never `build`).
- **Strict filing schema enforced by non-model code at filing time** (the existing
  human/wfe gate is downstream of this injection point) via a single
  `validate_proposal_path()` helper with **precisely-specified semantics** (mistral):
  gitignore-style area globs; `realpath`-resolve each path and require the result to
  stay under the repo root AND the area root (catches `../../.bashrc`, nested
  symlinks, `..` traversal); reject symlinks/hardlinks; reject shell-like syntax in
  free-form fields. The helper is unit-tested against a **corpus of malicious paths**.
  Survivors are **quarantined** (filed but flagged needs-human-sign). Per-area
  filed-item cap from WP-B2.

### WP-B6 — worktree + resumability + guardrails
- Fork a worktree off the **remote** default branch on `sweep/<slug>`; never local
  refs, never the default branch. The `sweep/*`-not-auto-built claim is **enforced in
  code config, not assumed (engineer)**: this PR adds/cites the CI exclusion (the
  `.github/workflows/*` branch filter line that excludes `sweep/*`) — if the CI
  manifest can't be edited here, the precondition is a no-push worktree only and the
  claim is dropped.
- **Pinned committer identity (engineer):** the commit helper sets
  `GIT_AUTHOR_NAME/EMAIL` + `GIT_COMMITTER_NAME/EMAIL` to a fixed `aimee-sweep
  <sweep@local>` and **refuses to commit** if they are unset or contain `${`/`$(` —
  the "non-model committer" is then a real, enforced identity, not the operator's
  ambient git config.
- **Drift state machine (engineer):** at area start record `tree_hash` (hash of the
  area's file list + contents); embed it in the area commit message; on resume, if
  the area's recomputed `tree_hash` differs from the last committed one, re-derive the
  exclusion map + area boundaries before filing.
- **Analysis-only**: writes only the per-area PRD + summaries; **no source edits**,
  **no push**. Staging by a fixed (non-model) committer that runs
  `git add -- <explicit path list>` (never a glob, never `-A`), each path passed
  through `validate_proposal_path()` (WP-B5) first; a **pre-commit hook fails the
  commit** if any staged path is off-allowlist or escapes the area root (enforcement,
  not just policy — mistral). Hook tested in `make unit-tests`.
- **Resumable**: per-area commit + an area checklist = the checkpoint; resume from
  the first unchecked area. On resume, if the remote tip moved, re-derive the
  exclusion map + area boundaries from the new tip; never file against a dead base.
- **Delta-aware**: a re-swept area excludes prior candidates; "zero new candidates"
  is a valid, honest result.

### WP-B7 — `aimee sweep` CLI + tests
- `aimee sweep [<path-or-subsystem>]` (default whole codebase) wires B2→B6; a
  per-area report table (Strong / Worth-exploring / Rejected / needs-manual).
- **Auth (engineer):** the CLI uses the existing per-user AIMEE session token for
  `/v1/delegate/run` + `/v1/dev/submit` — no new auth surface.
- Tests (pure where possible, fake backends like Part A), with **enumerated corpora**:
  - rule-of-three: `{3 callers, 0 shared, 3 files} → Strong`; `{3 callers, 2 shared}
    → needs-manual`; `{3 callers, all one file} → needs-manual` (distribution);
    `{2 callers} → Rejected`.
  - `validate_proposal_path` malicious corpus: `../../.bashrc`, `/etc/passwd`,
    `dir/../../../escape`, symlink-to-outside, hardlink-into-area, NUL byte,
    `; rm -rf`, `$(...)` — each rejected.
  - verify drop-on-contradiction + dedup-by-idkey; exclusion exact-key match +
    rename-bypass attempt; opaque-body injection filed byte-unchanged onto
    `manual-review`. `make unit-tests` green before push.

## Phasing within Part B
- **B-core (first PR):** B1 + B2 + B3 + B4 + B5 + B7, **plus worktree ISOLATION
  from B6**. The sweep is read-only w.r.t. source but it must NOT run in the shared
  checkout (concurrent actors — this exact hazard bit Part A): B-core forks a
  `sweep/<slug>` worktree off the remote default branch and writes the PRD there.
  (Resumability, delta-awareness, the drift state-machine, and the
  committer-identity hardening are the parts deferred to B-durable.)
- **B-durable (second PR):** B6 resumability + delta + content-hash drift + pinned
  committer identity + the pre-commit hook.

This means the worktree fork + fixed `git add -- <list>` staging ships in B-core
(isolation is a safety prerequisite, not a durability nicety); only resume/delta
machinery waits for B-durable.

## Out of scope (Part B v1)
- Auto-implement: the sweep is analysis-only; implementation stays the existing
  autonomous-dev path behind its gates.
- `ast_grep`/`EV_PATTERN` evidence (deferred in Part A; the db2 caller/blast-radius
  edges are the v1 deletion-test source).
- Scope-glob / expiry settled-decisions and content-hash site identity (Part A
  "deferred" list stands).

## Open (resolve at plan-gate / first run)
- Confirm the typed_facts read path from the DB2-disabled client (kb_client recall
  vs. server-side); if absent for `architecture_settled`, v1 exclusion uses
  proposals + work queue only and notes the gap.
- Tune the area-partition heuristic + caps against a first whole-codebase dry run
  (defaulted, calibration procedure from Part A's plan applies).
