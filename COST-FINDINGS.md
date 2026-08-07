# Cost and turn count — the aimee arm

Evidence for the article. Numbered findings, in the order they were established.
Each states what was measured, on what, and what it does *not* support.

Run context: 8 tasks × 4 arms (baseline / ponytail-instructions / ponytail-addon /
aimee), r1 only, disjoint lanes across CT 401/402/403 on `192.168.1.252`,
`codex exec --ephemeral --json`, `gpt-5.6-sol`, medium reasoning,
`agents.enabled=false`. **All 8 tasks complete in all 4 arms.**

---

## Finding 1 — The headline ratio was a token count, not a cost

Every "aimee costs 4.4×" figure produced before 2026-08-03 was a **raw
input-token ratio**. It charges cache hits at the full input rate. It is not a
cost, and it should not be published as one.

Cache hit rates here are 83–96% in every arm. On `am_e1af40a0f5`:

| arm | total in | cached | uncached | hit rate |
|---|---|---|---|---|
| baseline | 441,735 | 367,872 | 73,863 | 83.3% |
| aimee | 1,928,655 | 1,828,608 | 100,047 | 94.8% |

Raw-token ratio: **4.37×**. Uncached-token ratio: **1.35×**. The gap between
those two numbers is entirely cache accounting.

The harness never made this mistake — `codex_matrix_runner.py:744` already prices
cached input at 10% — but the analysis on top of it did, for several sessions.

## Finding 2 — The harness credit unit is exactly $0.04

`gpt-5.6-sol` published rates, effective 2026-07-30: **$5.00/MTok input,
$30.00/MTok output, cached input at 10% of input ($0.50/MTok)**.

The harness's `credit_rate_per_million` is `{uncached: 125, cached: 12.5, output:
750}`. Divide through:

```
5.00 / 125  = 0.04
0.50 / 12.5 = 0.04
30.00 / 750 = 0.04
```

All three agree, so **1 credit = $0.04** and USD is a clean rescaling of credits,
not a reweighting. The harness was built on the real Sol card even though it
records `usd_cost_available: False`.

**Do not use `benchmarks/coding/cost_savings.py` `DEFAULT_PRICE` for this work.**
That card ($1.25 in / $10.00 out) is self-described as an assumption for pricing
*free local models* at frontier-equivalent rates, it predates 5.6, and it carries
no cached tier at all — disqualifying when >90% of these tokens are cache hits.
It was used for one iteration of this analysis and understated dollars 4×.

## Finding 3 — Priced properly, aimee is 1.11x baseline

All 8 tasks, all 4 arms, at $0.04/credit:

| arm | USD | vs baseline |
|---|---|---|
| baseline | $14.218 | 1.00x |
| **ponytail-instructions** | **$13.044** | **0.92x** |
| ponytail-addon | $14.607 | 1.03x |
| **aimee** | $15.729 | **1.11x** |

Per task, aimee is cheaper than baseline on **four of eight**:

| task | base | p-instr | p-addon | aimee | aimee vs base |
|---|---|---|---|---|---|
| am_e1af40a0f5 | $0.662 | $1.242 | $0.987 | $1.729 | 2.61x |
| am_1f0f1ab528 | $0.661 | $1.234 | $0.984 | $1.090 | 1.65x |
| am_12b43fa38e | $1.750 | $2.487 | $1.681 | $2.829 | 1.62x |
| am_b84c9294aa | $4.061 | $2.625 | $4.015 | $4.606 | 1.13x |
| am_270b3483d5 | $1.260 | $0.880 | $1.403 | $1.097 | 0.87x |
| am_1e7cb3da16 | $1.632 | $1.172 | $2.091 | $1.412 | 0.87x |
| am_e4c4afa194 | $1.159 | $0.858 | $0.788 | $0.971 | 0.84x |
| am_312e901904 | $3.033 | $2.546 | $2.657 | $1.996 | 0.66x |

Note ponytail-instructions comes in **below** baseline over the full set -- the
only arm that does.

Earlier drafts of this file recorded 1.28x (6 cells) and 1.10x (7 cells). The
figure moved with every added cell, which is itself the caveat: at n=8 with one
replicate, the aggregate is not stable to a single task.

## Finding 4 — aimee already wins on bytes and on uncached input

Two measurements that cut against the "aimee is expensive" framing. Credit totals
here are over the **first six** cells (Finding 3's table before `am_312e901904`
landed); the ordering they establish is unchanged by the seventh:

- **aimee's uncached input is the lowest of all four arms** — 65.0 credits against
  baseline's 69.8 and ponytail-addon's 77.8. On the tokens billed at full rate,
  aimee is the cheapest arm in the study.
- **aimee moves the fewest tool-output bytes.** On `am_e1af40a0f5`: aimee 74,457
  characters against baseline 123,647, ponytail-instructions 199,955,
  ponytail-addon 234,090.

The retrieval work (`index` in the core tool floor, hybrid over recursive grep,
span over whole-file reads) did what it was built to do. The byte war is won, and
it did not move the bill.

## Finding 5 — All of aimee's overhead is turn count

Every dollar of aimee's excess sits in cached input (122.8 cr vs baseline's 79.4)
and output (40.2 vs 28.9), over the first six cells. Cached input is turns ×
prefix: across all seven, 259 calls against baseline's 113.

On `am_e1af40a0f5`, 47 calls vs 9. Per call, aimee is *cheaper* than baseline
(41.0k input-tokens vs 49.1k) because its context carries less. It simply takes
five times as many turns.

**Turn count is the entire remaining cost story.** Output volume is not the lever;
it was, and it has been taken.

## Finding 6 — Cost ratio is ~0.5 x call ratio

**A "fixed exploration floor" was claimed here on 7 cells and the 8th refuted
it.** `am_b84c9294aa` took 58 aimee calls against baseline's 28 -- above the 19-58
band that had looked like a ceiling -- and its cost ratio (1.13x) does not sit
where task difficulty predicts. Sorting by baseline effort is NOT monotone.

Sorting by **call ratio** is:

| task | base calls | aimee calls | call ratio | cost ratio | cost/call ratio |
|---|---|---|---|---|---|
| am_312e901904 | 31 | 38 | 1.2x | 0.66x | 0.55 |
| am_270b3483d5 | 15 | 19 | 1.3x | 0.87x | 0.67 |
| am_e4c4afa194 | 13 | 19 | 1.5x | 0.84x | 0.56 |
| am_1e7cb3da16 | 19 | 37 | 1.9x | 0.87x | 0.46 |
| am_b84c9294aa | 28 | 58 | 2.1x | 1.13x | 0.54 |
| am_12b43fa38e | 17 | 58 | 3.4x | 1.62x | 0.48 |
| am_1f0f1ab528 | 9 | 41 | 4.6x | 1.65x | 0.36 |
| am_e1af40a0f5 | 9 | 47 | 5.2x | 2.61x | 0.50 |

**cost_ratio ~= 0.5 x call_ratio**, holding from 1.2x to 5.2x -- a factor-of-four
range -- with the multiplier between 0.36 and 0.67 and no trend in it.

The 0.5 is the per-call discount from Finding 4: aimee's context carries less, so
each call is cheaper, so N x the calls costs about N/2 x the money.

**The actionable form: halve aimee's call count and you roughly halve its cost
ratio.** aimee does have a floor -- it never drops below ~19 calls even when
baseline uses 9 -- but it is not a ceiling, and difficulty does not predict it.
What predicts cost is simply how many calls aimee takes.

## Finding 7 — The guidance traded bytes for turns

This is self-inflicted and is the most useful finding in the set.

The skill text was rewritten to steer reads to `index command=span` and searches
to `index command=hybrid`. Both are bounded and both cut bytes. But `span` reads
**one range per call**, and the shell form it displaced batched several reads into
a single command.

Call-mix on `am_e1af40a0f5`:

- **baseline: 16 `sed` reads inside 9 calls.** One call is
  `git status && sed … && sed … && sed … && sed …`. Another does four more.
- **aimee: 7 `sed` reads across 22 calls**, plus 25 MCP calls. Almost no chaining.

Of aimee's 22 shell calls, roughly nine bought nothing:

| # | call | output |
|---|---|---|
| 1 | `sed -n '1,240p' …/plugins/cache/local/aimee/…` — reading its own SKILL.md | 2,714 ch |
| 5, 8 | searches returning nothing | 0 ch |
| 6, 16 | searches returning a single path | 35 ch |
| 3, 20 | lone `git status --short` | 0 / 89 ch |
| 21 | lone `git diff --check` | 0 ch |
| 19 | lone `unlink /tmp/…` | 0 ch |

Baseline folded its `git status` and `git diff --check` into commands it was
already running. aimee spent a turn on each.

Anatomy of the 47 calls on `am_e1af40a0f5`, the worst cell:

- **11 `span` calls, one range each.** `src/config.c` alone is read at :1-120,
  :205-245, :850-1035, :1030-1175 and :1585-1625 in five separate round trips.
- **5 consecutive `find_symbol` calls**, one symbol each.
- **14 consecutive MCP calls before any shell work** (calls 4-17), then 9 more
  (19-27) -- a long chain of single-purpose probes.
- **9 shell calls that bought nothing** (table above).

~16 of 47 calls are inherently batchable and ~9 are waste. Removing both lands
near 25 calls, which on the observed call-to-cost relationship is roughly
1.3-1.4x rather than 2.61x. **Projection, not a measurement.**

**Fixes landed** (all red-before-green verified):
- `code_span_get` accepts a `spans` array — `[{file_path, line_start, line_end}, …]`
  — returning every range in one call; schema advertises it, tools-list golden
  regenerated.
- Skill gains: put independent commands in ONE call joined with `&&`; batch reads;
  fold `git status` / `git diff --check` / cleanup into a command already running;
  use `spans` for more than one range; do not read the plugin cache.

**Not yet measured.** These were built after the r5 suite launched. Effect size is
unknown and must not be asserted until a matched re-run exists.

## Finding 8 — The output price barely matters; the cached rate does; task
selection dominates both

Sweeping the two parameters we were unsure of, holding input at 1.0 (it scales
dollars but cancels in the ratio):

| box | tasks | aimee vs baseline, across the whole sweep |
|---|---|---|
| CT401 | am_1f0f1ab528, am_e1af40a0f5 | 1.82× – 3.30× |
| CT402 | am_12b43fa38e, am_e4c4afa194 | 1.19× – 1.59× |
| CT403 | am_1e7cb3da16, am_270b3483d5 | 0.86× – 0.90× |

- Output multiple from 4× to 15× moves any ratio by **≤0.05×**. Not knowing the
  output rate costs nothing.
- The cached discount swings CT401 from 1.82× to 3.30×. It is the one parameter
  worth pinning down, and it is now pinned (Finding 2).
- **The same aimee build reads 2.24× on one task pair and 0.87× on another at one
  fixed card.** Per-task variance is larger than the entire pricing uncertainty.

This is the strongest caveat in the set: "what does aimee cost" is not answerable
from 6 tasks at one replicate, whatever the rate card.

## Finding 9 — A bullet added on intuition, measured at zero

The skill carried "Do not repeat a search you have already run." Measuring
duplicate searches across all arms afterwards: **0–1% of work**. It bought
nothing and occupied skill budget.

Removed, and the test now asserts it stays *absent* so it is not reintroduced on
the same intuition. Guidance added without a measurement should be treated as
unproven regardless of how obvious it sounds.

---

## Finding 10 — read_symbol exists, fuses the two-call pattern, and is unreachable

The agent's commonest read shape is two calls: `find_symbol` to get a range,
then `span` to read it. `read_symbol` already does both server-side --
*"Fetch just a symbol's definition span (anchored, editable) instead of reading
the whole enclosing file"* (`server/agent_tools.c`).

It has **no MCP presence at all**: not in `tools/list`, not in the extended
catalog, not reachable through `find_tools`/`call_tool`. A Codex agent cannot
call it by any path.

This is the same shape as the `index` finding -- the right tool existed and was
never offered, so the agent paid for the long way round. Worth stating plainly
in the article: twice now, the measured "agent behaves inefficiently" turned out
to be "the efficient tool was not on the menu."

**Not fixed, deliberately.** Batched `find_symbol` + batched `spans` resolves N
symbols in 2 calls, where N `read_symbol` calls would cost N. Exposing it would
be the worse fix for the case that actually occurs.

## Finding 11 — THE BLOCKER: aimee's work is invisible to the grader when hooks are on

**Two consecutive runs of `am_e1af40a0f5` scored 0 LOC and failed. The agent had
done the work correctly both times.**

- run A: `src/config.c`, `src/config_internal.h`, `src/config_save.c` — 16 insertions
- run B: same three files — 29 insertions

Both patches sat in the cell's session worktree. The harness diffs the **cell
root**, which is clean, so `patch.diff` is 0 bytes and `hidden_ok` false. A full
re-run in this state would have scored aimee **0/8** and read as catastrophic
regression, with every patch real and one directory down.

**Every cost figure measured in this state is void**, including the
"2.61x -> 1.95x" improvement reported for batching: that run produced no graded
patch, so it was cheap partly because it skipped the edit/verify cycle.

### What is and is not established

Established: the r5 results (Findings 1-9) predate hook deployment and are
unaffected. The regression appeared only after `hooks pre` was registered.

NOT established: what creates the worktree. It could not be reproduced in
isolation — `mcp-serve`, `hooks pre` (PreToolUse and SessionStart payloads) and
`workspace add` were each driven directly in a throwaway git repo, with
`AIMEE_HOME` pointed at the same config, with and without `AIMEE_SESSION_ID`, and
none created one. In the cell it appears in the **same second the cell directory
is created** — during harness setup, before the agent runs. The corpora do not
ship a stale registry. `require_session_worktree: false` is set and verified
present, and the standalone probe honours it; the cell does not.

**Root cause open.** The benchmark is unblocked by `require_aimee_git: false`
(the deny message's own documented opt-out), which removes the only thing that
changed between the valid r5 runs and the 0-LOC ones. That is a workaround, not a
fix, and it disables the git redirect that was under test.

### Why this matters beyond the benchmark

An MCP client hands aimee a checkout and expects edits in it. aimee relocates
them to a branch in a worktree the caller never learns about from any tool
result. For a benchmark that is a scored zero; for a user it is "the model said
it fixed it and my repo is unchanged."

A related defect WAS root-caused and fixed: the `initialize` instructions said
"use RELATIVE paths" for a worktree only *aimee's* tools had moved into. The MCP
host's own shell never moved, so relative paths from it land in the shared
checkout the same text forbids editing. An agent given that spent nine calls
locating the worktree, then prefixed every shell command with an absolute cd. The
text now names both surfaces separately.

## Finding 12 — no task in the current eight is an aimee-only win

Pass matrix, all four arms, r5:

| task | baseline | p-instr | p-addon | aimee |
|---|---|---|---|---|
| am_1f0f1ab528 | PASS | PASS | PASS | PASS |
| am_312e901904 | PASS | PASS | PASS | PASS |
| am_e4c4afa194 | PASS | PASS | PASS | PASS |
| am_270b3483d5 | fail | fail | **PASS** | **PASS** |
| am_12b43fa38e | fail | fail | fail | fail |
| am_b84c9294aa | fail | fail | fail | fail |
| am_1e7cb3da16 | fail | fail | fail | fail |
| am_e1af40a0f5 | PASS | PASS | PASS | (void, Finding 11) |

The closest differentiator is `am_270b3483d5`: aimee and ponytail-addon pass,
plain codex and ponytail-instructions fail. Real, and NOT aimee-only.

## Finding 13 — two candidate tasks built, selected for structure not outcome

Tasks derive from real fix commits (`am_` + first 10 hex of the SHA), so adding
one is reproducible: corpus checkout at the commit's PARENT, upstream test files
injected at grade time, the non-test diff as reference patch, and a ticket to the
same standard as the others (observed failure plus diagnosis, no file names).

**`am_edb3594485`** — already had a hidden test and reference patch and no
ticket; ticket written, now running. Chosen because SYMPTOM and CAUSE are in
different files with no shared literal: the operator-visible warning is emitted
in `src/posix/agent_runtime.c`, the defect is in `src/server/agent_bridge.c`
(streamed `function_call` items collected then discarded). Grepping the message
lands in the file that PRINTS it, not the file that must change.

**`am_4aec72896d`** — built tonight from `4aec72896d24`, "close-on-exec accepted
sockets". Chosen because the fix requires recognising the SAME defect class in a
second, independent service: aimee-kb's mTLS listener had it, its plaintext
sibling was already correct. Symptom (`aimee workspace add` hanging 28 minutes)
is remote from cause (fd inheritance across fork). Its upstream commit also adds
the make rule for its test binary, so the graded test APPENDS that rule rather
than substituting the file. **Not yet validated red→green.**

Selection was on structure — cause/symptom distance, second-site discovery —
decided before any arm ran. Whether either separates the arms is open, and a task
selected because aimee wins would measure what aimee is FOR, not that it is
better. The article must state the selection rule either way.

## Finding 11-CORRECTED — the 0-LOC failures were a self-inflicted instruction change

The earlier text in this section blamed hook registration and then a client
regression. **Both were wrong.** Recording the correction because the wrong
diagnosis cost several hours and the right one took a single comparison.

**Actual cause:** commit `63603c1a5` rewrote the MCP `initialize` instructions to
tell the HOST's shell to use absolute paths under aimee's session worktree, or to
`cd` there. That was accurate about where aimee's own tools run, and it moved the
agent out of the checkout the caller owns.

**The evidence that settles it** — compare a passing cell with a failing one:

| cell | worktree present | edits at root | edits in worktree | graded |
|---|---|---|---|---|
| r5 passing (`am_1f0f1ab528`) | **yes** | 2 files | 0 | PASS |
| after `63603c1a5` | yes | 0 | 3 files | 0 LOC, FAIL |
| after revert (`f5468e9ce`) | yes | 2 files | 2 files | **PASS, 25 insertions** |

The worktree is created in **every** era. Its presence was never the problem;
where the agent chose to WRITE was. The harness diffs the directory it handed
over, which is the only directory any caller can be expected to look at.

**What the wrong hypotheses cost.** Hook registration was blamed first
(`require_aimee_git: false` changed nothing). Then a client regression, which
triggered a bisect: `mcp-serve`, `hooks pre` (PreToolUse and SessionStart),
`workspace add` and `index scan` were each driven directly against both binaries,
with and without an origin remote and a session id. **Both clients behaved
identically in every probe.** The binary was never the variable.

The check that found it — diff a passing cell against a failing one — was
available the entire time and takes one command. Reach for it before bisecting.

**Still unexplained, now decoupled from grading:** `require_session_worktree:
false` does not prevent worktree creation in a real cell, although every isolated
probe honours it. And after the revert the agent writes to BOTH root and
worktree. Neither blocks measurement; both are loose threads.

## Finding 14 — the three all-fail tasks are one failure: under-scoped patches

All three are solvable (each reference patch passes its own graded test), so the
four-way failures are real capability failures. In every one, retrieval was
CORRECT and the patch was too narrow:

| task | ticket said | aimee did |
|---|---|---|
| am_b84c9294aa | a lease taken and never returned | ran `find_callers` on `db2_lease_begin`/`db2_lease_end`, read `db2_init.c` (a reference file) — then patched 7 lines in ONE consumer, where the reference makes the pool reclaim |
| am_1e7cb3da16 | a three-link chain | changed 2 of the 5 files |
| am_12b43fa38e | **"Two bugs"**, both named | fixed the second, never touched the first — while editing that defect's file for an unrelated reason |

`am_12b43fa38e` is the sharpest: file overlap with the reference looked like
coverage and was not. Judging coverage by which files were touched is wrong.

**This is not a retrieval gap.** The index found the right code every time. The
gap is that the author cannot see what they omitted.

Two responses shipped, deliberately separable so each can be measured alone:

1. **Guidance** (`571c71e78`) — fix the OWNER not one caller (`find_callers`
   gives the caller count, so N>1 makes a caller-side fix incomplete by
   construction); account for every symptom the ticket names.
2. **`review_completeness`** (`f986334bb`) — a DELEGATE reviews the tree against
   the requirements and returns each stated defect ADDRESSED / NOT ADDRESSED plus
   a COMPLETE/INCOMPLETE verdict. Separate context, own persona, configurable via
   `completeness_review_persona`. It reads the tree itself, so a mis-stated diff
   cannot hide the omission being looked for.

**Delegate token accounting.** The harness meters the codex transcript only, so
delegate tokens are invisible to it by construction. That is correct when
delegates run on a free local fleet and MISLEADING on a paid one — aimee would
look cheaper purely by moving work where the meter cannot see it. Report delegate
cost alongside primary cost; never fold it in silently, and never omit it.

## Finding 15 — five times, the capability existed and nothing pointed at it

The single most repeated result of this investigation is not a cost number. It is
that aimee already had the thing, and the agent could not reach it or was never
told:

| # | capability | why it went unused |
|---|---|---|
| 1 | `index` (hybrid/structure/span/callers) | not in the MCP core tool floor — reaching it cost find_tools → describe_tool → call_tool, so the agent grepped instead |
| 2 | `read_symbol` | **no MCP presence at all** — not in tools/list, not in the extended catalog, unreachable by any path |
| 3 | `/v1/code/context` (bounded task packet) | wired ONLY as ingress pre-injection and for delegates; an MCP agent could not call it |
| 4 | `roundtable_review` | present and never pointed at, so a duplicate `review_completeness` tool was nearly shipped |
| 5 | session-worktree handoff | the isolation exists; what the caller was TOLD about it changed the outcome from pass to zero |

Every one of these showed up in transcripts as "the agent explores inefficiently"
or "the agent under-fixes". None of them was a model capability problem.

`roundtable_review` is the sharpest case because the duplication was caught only
by reading the schema. It already provides everything the new tool was being
built for: every seat is an ordinary delegate request (a one-seat panel IS a
single reviewer with a persona), `original_request` is documented as *"used to
detect goal drift"* — which is precisely the measured failure, a change that is
reasonable but is not the change that was asked for — `brief` carries
focus/fixes/invariants/questions, and `workdir` hands the reviewer the checkout.
`rt_preset_t` carries seats[]/seat_count with per-seat persona plus chairman,
min_successful, max_cost_usd and deadline_ms, and presets are a server-owned
registry created over /v1. A named one-seat "completeness" preset is the
configurable form and needs no new code.

Shipping a second review path would have discarded chairman synthesis, evidence
requirements and cost caps to re-solve a solved problem.

**For the article.** The interesting claim is not "aimee is 1.11x baseline". It is
that a framework's measured performance was dominated, repeatedly, by tool
DISCOVERABILITY rather than tool capability — and that the fix each time was to
put an existing thing on the path the agent actually walks. That is a
generalisable finding about agent frameworks, and it is falsifiable: each fix has
a before/after transcript.

**Accounting note for the roundtable path.** A review seat is a delegate, and
delegate tokens never appear in the codex transcript the harness meters. Report
seat cost separately rather than letting review work vanish into an unmetered
channel — the same discipline as Finding 14. (Until Finding 16 the polling turns
DID appear and were billed to the primary; the review is synchronous now, so the
only metered turn is the single blocking call.)

## Finding 16 — the roundtable poll contract, and why polling is never cheap

**Counting note, and a correction.** `summary.json`'s `codex.tool_calls` counts
`item.started` AND `item.completed` for the same item, so every call type that
emits both (command_execution, mcp_tool_call, file_change) is DOUBLED there.
This finding originally quoted those doubled figures. Count distinct item ids
from `item.completed` instead. Token usage is NOT affected: exactly one event
per run carries `usage` (`turn.completed`), so every credit figure below and
elsewhere in this document stands.

`aimee__am_1f0f1ab528__r1` (CT403), distinct tool calls:

```
shell (command_execution)      19
mcp:aimee:roundtable_status    16   <--
mcp:aimee:index                 2
mcp:aimee:preview_blast_radius  2
mcp:aimee:find_symbol           1
mcp:aimee:roundtable_review     1
file_change                     1
                            -----
                               42   (baseline: 10)
```

**16 of 42 calls were polling one review to completion.** Actual retrieval — the
thing aimee is supposed to be paying for — was 5 calls.

Measured, not inferred:

| | baseline | aimee |
|---|---|---|
| uncached input | 68,295 | 94,458 (1.38x) |
| cached input | 423,168 | 2,705,152 (6.39x) |
| credits | 16.52 | 53.49 |
| — cached component | 5.29 | **33.81 (63% of the bill)** |

The retrieval context aimee pulls in is cheap: uncached input is only 1.38x
baseline. The cost is cached input, and cached input scales with TURN COUNT,
because every turn re-sends the whole accumulated conversation. Across all four
arms, credits are monotonic in call count (10/16/22/42 calls →
16.52/24.61/30.84/53.49 credits).

**The mechanism.** A poll costs microseconds server-side and an entire model
turn on the client. The tool description said, in as many words:

> "Returns a run_id immediately; poll roundtable_status until synthesis completes."

and the submit path attached `poll_after_ms: 1000` to every non-terminal
snapshot. A one-second poll interval, advertised on a job whose own header
comment says it can run twenty minutes. The agent was following instructions.

**The part that makes this a design finding rather than a tuning one.** The
layer below was already synchronous: `handle_roundtable_review` calls
`obs_bus_module_call` and blocks on the bus until the verdict arrives. The
asynchronous op-run wrapper on top of it made nothing concurrent that was not
already concurrent — it only moved completion-detection to the one place where
waiting is expensive, the model's turn loop.

Fixed by deleting the wrapper: `roundtable_review` dispatches inline and returns
the verdict. Each layer blocks on the one below —

```
thin client -> aimee-server -> event bus -> roundtable -> model
```

— and none of it is provider-specific. `roundtable_status` is gone from the tool
surface entirely; a status tool is an invitation to poll even when the call
already blocks.

**Not yet measured.** The post-fix cost is a rerun, not an arithmetic exercise:
polls land late in a run when context is fattest, so 16/42 calls is not 38% of
the bill, and the blocking call's wall time is a real (if unbilled) change.
Report the new cell, do not project this one.

**What the fix does NOT address.** Removing the polls takes the cell from 42
calls to 26, against baseline's 10. The remaining +16 is Finding 17.

## Finding 17 — WITHDRAWN, and replaced by Finding 19

**This finding was wrong and its correction matters more than the original.**

It claimed aimee "paid for the index and still grepped" -- retrieval additive
rather than substitutive, 12 exploration shell calls against baseline's 5. The
mechanism was not additive retrieval. `index command=investigate` ABSTAINED:

```json
{"results":[], "status":"abstained", "item_count":0,
 "answerability":{"decision":"no_answer","reason":"no_evidence_above_floor",
                  "candidate_count":0, "top_confidence":0, "vector_floor":0.7}}
```

`code_embeddings` held **zero rows, globally, for every project that has ever
existed in this deployment**. The agent asked semantic search the right
question, got "no answer", and fell back to grep. It behaved correctly; the
capability was absent.

Retained below as the original text, because the numbers in it are real and the
conclusion drawn from them was not. See Finding 19 for the cause.

### Original text (conclusion withdrawn)

### Finding 17 — the rest of the turns: retrieval was additive, not substitutive

Same cell, with the 16 polls removed: **26 calls against baseline's 10.** Where
the remaining +16 sits, by category:

| category | baseline | aimee | delta |
|---|---|---|---|
| MCP retrieval (index/find_symbol/blast_radius) | 0 | 5 | +5 |
| roundtable_review (blocking verdict) | 0 | 1 | +1 |
| exploration shell (search, read, search+read) | 5 | 12 | **+7** |
| git inspection | 1 | 3 | +2 |
| build/test | 3 | 3 | 0 |
| file write | 1 | 1 | 0 |
| read own SKILL.md from the plugin cache | 0 | 1 | +1 |

**The finding is the third row.** aimee spent 5 calls on the index — and then did
**12 exploration shell calls to baseline's 5**, on the same task. It grepped the
tree *more* than the arm with no index at all. Retrieval did not replace search;
it was added on top of it. That is 7 turns, and it is the largest remaining
block after polling.

This is the same shape as Finding 7 but a different mechanism. Finding 7 was
about *batching* (span reads one range per call where the shell form chained
several). Here the shell calls ARE well batched — five of the twelve chain two
or three reads with `&&`, and the three build/test calls exactly match
baseline's. The waste is not in how the calls are formed; it is that the
retrieval and the searching answer the same question and both get asked.

Two calls bought nothing at all:

- **#1** reads `…/plugins/cache/local/aimee/0.3.0/skills/…` — the agent reading
  its own skill file. `client_integrations.c` tells it not to, in those words
  ("Do not read this file, or anything else under the plugin cache"), and it did
  anyway. This same call appears in Finding 7's waste table on a different task,
  so it is not a one-off.
- **#18** re-reads `TICKET.txt` at the end of the run, after **#2** had already
  read all 240 lines of it.

**What this does not say.** n=1 cell, one task, index-only mode. The exploration
gap needs to hold across the corpus before it is a claim; a single task where
the index answers badly would produce this pattern honestly. Check it against
the other 13 before publishing.

## Finding 18 — "no arm wrote tests" is a harness bug, not a result

`codex_matrix_runner.py:672`:

```python
bucket = "test" if rel.startswith("tests/") else "production"
```

The am_ corpus keeps its tests at **`src/tests/`**, not `tests/`. The prefix never
matches, so every test file in every arm is bucketed as production and
`test_added` is structurally 0.

`aimee__am_1f0f1ab528__r1` is the proof — its own `per_file` contradicts its
totals:

```
{'added': 40, 'deleted': 5, 'kind': 'production', 'path': 'src/tests/test_bus_capture.c'}
...
'production_added': 71,  'test_added': 0
```

40 of those 71 "production" lines are test code. Real production is ~31.

**Two published claims die here.** "No arm wrote tests on a corpus built from
real fix commits" was my statement and it is wrong. And every
`production_added` figure in this document is inflated by whatever test code
that cell wrote — which is exactly the axis the article wants to compare.

**Do not fix the runner mid-experiment**; changing the metric between cells makes
early and late cells incomparable. Recompute post-hoc from each cell's retained
`patch.diff` instead, bucketing on `"/tests/" in path or basename.startswith("test_")`,
and restate the LOC table from that.

### Recomputed from patch.diff — every arm wrote tests

Harness said `test_added: 0` for all 25 cells. Recomputed:

| arm | cells | prod+ | test+ | cells that wrote tests |
|---|---|---|---|---|
| baseline (CT403) | 4 | 199 | 166 | **4/4** |
| ponytail-addon (CT403) | 4 | 85 | 72 | **4/4** |
| ponytail-instructions (CT403) | 4 | 72 | 66 | **4/4** |
| aimee | 1 | 31 | 40 | **1/1** |
| baseline (CT401) | 4 | 81 | 17 | 1/4 |
| ponytail-addon (CT401) | 4 | 44 | 13 | 1/4 |
| ponytail-instructions (CT401) | 4 | 53 | 13 | 1/4 |

**`am_1f0f1ab528` — the one task with all four arms, and the one this document
dissects throughout:**

| arm | prod+ | test+ |
|---|---|---|
| baseline | 30 | 0 |
| ponytail-addon | 17 | 0 |
| ponytail-instructions | 16 | 0 |
| **aimee** | **31** | **40** |

aimee is the only arm that wrote a test on this task, at production parity with
baseline (31 vs 30). Under the broken metric this read as "aimee wrote 71
production lines to baseline's 30" — 2.4x the code for the same fix, which
sounds like bloat. Corrected, it is 31 lines of production plus a test the other
three arms did not write.

**This is the single largest correction in this document.** The quality axis the
article was built to measure was reading zero for everyone, and the one arm-level
difference visible in it points the opposite way from the cost story. n=1 on the
four-arm comparison; check it across the corpus before publishing.

## Finding 19 — the aimee arm has never had a code vector

`SELECT count(*) FROM code_embeddings` returns **0**. Not for the cell under
test -- for every project, across the whole deployment, for the life of this
study.

The cause is a gap between two paths that were supposed to do the same build:

| | doc vectors | canonical code index | code vectors |
|---|---|---|---|
| async worker (`kbiw_process_job`) | yes (`kb_build`) | yes | **never** |
| sync HTTP (`/v1/code/build`) | yes | yes | yes |

`kb_code_embed_refresh` appeared zero times in the worker. Every project arrives
through the queue, so no project ever got code vectors. The benchmark then ran
with `PT_SKIP_KB_BUILD=1`, so the one path that did build them was never called
either.

**What this contaminates.** Every aimee cell in this study ran with semantic
code search disabled. The arm's measured turn count and credit cost are the cost
of an agent falling back to grep, not the cost of the retrieval design. Finding
17's exploration gap, and any share of Findings 5/6/7 attributable to discovery
turns, are affected. Findings that do not depend on semantic search -- the
roundtable polling (Finding 16), the LOC classifier (Finding 18), the credit
arithmetic (Findings 1-3) -- stand.

**What it does NOT explain.** The 16 roundtable polls were a real defect of
their own. So were the plugin-cache read and a blast-radius query 139x slower
than necessary. "Embeddings were empty" is not a universal excuse, and the
temptation to treat it as one should be resisted.

**Unmeasured.** Whether working semantic search actually reduces turns is a
hypothesis, not a result. It could plausibly increase follow-up reads instead.
The honest position is that the comparison has not been run yet: no aimee cell
to date is a valid measurement of aimee's retrieval, and the first one that will
be is the rerun on the build that fixes this.


## Finding 20 — the review was asked exactly the right question and rubber-stamped it

On am_312e901904 the agent invoked `roundtable_review` correctly and with a
better brief than most humans would write:

```json
"original_request": "...TICKET.txt: Two defects behind the same report..."
"brief": {"focus": [
   "Verify both distinct defects in the ticket are actually fixed",
   "Check fd remapping/chdir ordering for regressions",
   "Check bulk-clone selection and inventory reconciliation in both UIs"]}
"artifact_stage": "frozen_diff"
```

The first focus item names the exact failure. The patch under review fixed
defect 1 and misdiagnosed defect 2 (it guessed stale client selection state; the
real bug was repos filed under the browsed owner instead of their own
clone_url, in server_http_routes_git.c, which the patch never touches).

The panel returned:

```json
"approved": true,
"artifact": "Roundtable approved the artifact with no findings.",
"participants_total": 1, "participants_used": 1, "cost_usd": 0
```

**The configured default panel is one seat.**

```json
"seats": [{"model": "$random", "persona": "qa"}],
"chairman": "$random", "chairman_enabled": true,
"min_successful": 1, "max_cost_usd": 0
```

A roundtable of one is a reviewer, not a roundtable, and the chairman has
nothing to synthesise. `cost_usd: 0` is unexplained: a real model review is not
free, and no delegate or convene line appears in the server log for the run id.
Whether the seat ran a model and missed the omission, or never ran, is NOT yet
established -- and the two have very different fixes.

**Why this is the most actionable finding in the document.** Every other defect
found in this study was in plumbing. This one is in the mechanism that is
supposed to be aimee's advantage. The agent did its part: it froze the diff,
passed the full ticket as original_request (which the tool documents as
goal-drift detection -- "the change is reasonable but is not the change that was
asked for"), and briefed the panel to check both defects. The review failed open.

Fixing it is the difference between an arm that ships half a ticket and an arm
that catches its own omission, and it costs nothing in agent turns because the
call is already being made.

## Finding 21 — for the identical deliverable, aimee is 27-39% cheaper

am_312e901904 is the cleanest cost comparison in the study, because three of the
four arms produced the SAME artifact. Counted with benchmarks/loc_real.py, which
excludes comments, includes and relocated code:

| arm | files | new code | test code | credits |
|---|---|---|---|---|
| baseline | 1 | 0 | 0 | 75.82 |
| ponytail-instructions | 1 | 0 | 0 | 63.66 |
| ponytail-addon | 2 | 1 | 0 | 66.43 |
| aimee | 4 | 27 | 19 | **46.25** |

The graded fix is an ORDERING change: move two statements in src/posix/util.c so
chdir runs before the fd remap. All four arms made exactly that change, and it
is zero net lines of new code in every one of them. baseline and
ponytail-instructions produced nothing else at all; ponytail-addon added a single
Cache-Control header.

So for the identical shared deliverable -- the only thing the grader scores --

    aimee 46.25 vs 63.66 (p-instructions)  =  0.73x, 27% cheaper
    aimee 46.25 vs 66.43 (p-addon)         =  0.70x, 30% cheaper
    aimee 46.25 vs 75.82 (baseline)        =  0.61x, 39% cheaper

and aimee ALSO shipped a 19-line regression test that is empirically verified to
catch the defect (replayed on the pristine corpus it aborts on the rc=127
assertion), plus 27 lines attempting the ticket's second defect. The other three
shipped no test at all.

**Why this is the comparison worth publishing.** Elsewhere in this document
aimee's cost is confounded: it does more work, so "more expensive" and "more
output" are entangled and 1.11x aggregate means little. Here the common
deliverable is byte-comparable across arms, so the cost difference is not paid
for by doing less. It is cheaper for the same work AND does more with the
remainder.

**The caveats that must travel with it.** n=1 task, one replicate. Delegate seat
tokens do not appear in the codex transcript the harness meters, so aimee's
roundtable review is real work that is NOT in the 46.25 (see the accounting note
under Finding 15) -- the number is an undercount of aimee's true spend, and the
comparison should say so rather than bank the gap. And the spread among the
three non-aimee arms for identical output (63.66 to 75.82, a 19% range) is a
useful reminder of how much run-to-run noise a single replicate carries.

## Methodology traps hit in this work

Record these; several produced wrong published numbers first.

1. **Template staleness voided an entire A/B series.** `prepare_templates()` gated
   on `provenance.json` merely existing, so templates built at 09:50 were served
   to every later cell despite four CLI installs. The A/B guidance results
   (72.8 → 39.8 → 29.6 credits) are **void**. The harness had been emitting
   `"aimee version moved after preparation"` for hours; it was dismissed as noise.
   Fix compares `pinned.get("client_sha256")` against the installed binary.
   *Verify the stack under test before trusting any delta.*

2. **Mismatched task sets across arms.** Two aimee cells were still in flight and
   recorded zero usage. Dropping them from aimee alone while leaving them in
   baseline compared different task sets and produced a fake 0.62×. A task must be
   dropped from **every** arm or none.

3. **In-flight cells look like broken cells.** `am_312e901904` showed 5 calls and
   0 tokens, then 28 calls and 0 tokens. Usage lands at completion. Check call
   count movement before calling a cell broken.

4. **`am_12b43fa38e`'s ticket was truncated mid-enumeration** (165 chars, ending on
   a colon that promised two bugs and named neither). Every arm found the right
   files and failed the graded test — a spec gap, not a capability gap. Repaired to
   633 chars from the reference patch, at the same level of detail as the other
   seven tickets. **Cells run before and after the repair are not comparable, in
   any arm.**

5. **A re-measurement that silently returned the OLD numbers.** `run_cell()`
   skips any cell whose `complete.json` exists at the current artifact schema and
   reports `{"status": "skipped"}`. A re-run launched without `--force` therefore
   completed in seconds and printed a per-arm summary block that looks exactly
   like a fresh result (`credits: 120.35`) but is the aggregate of the PRIOR
   cells. Always pass `--force` when re-measuring, and confirm the raw
   transcript's mtime moved before reading any number from it.

6. **`pgrep -f <pattern>` matches the watcher that contains the pattern.** A
   background `until ! pgrep -f codex_matrix_runner` loop matches its own command
   line and never exits, making a finished run look like a running one for
   45 minutes. Use a bracket class (`pgrep -af "[c]odex_matrix_runner"`). Also
   note `pgrep` without `-f` matches only the process NAME -- for `python3
   codex_matrix_runner.py` that is `python3`, so `pgrep -c codex` returns 0 and
   means nothing.

7. **Instrumentation bug, unresolved.** `seq.py` reads tool output from
   `aggregated_output` / `output`, which is absent on `mcp_tool_call` items, so it
   reports 0 characters for every MCP call. An earlier script measuring
   `len(json.dumps(item))` found mean 2,545 chars, max 12,201. **Do not cite MCP
   result sizes from the call-sequence dump.**

## Finding 22 — two operating points, and aimee wins at both

Re-run of am_312e901904 on the aimee arm (aimee-kb:rt47 / aimee-server:rt48,
2026-08-06), with the corpus indexed once and the test gate live for the first
time. Graded result:

    hidden_ok    true
    tests_ok     true      reason: catches_defect
    tests_files  frontend/src/setup/ownerUrl.test.ts, src/tests/test_git_cred_inject.c
    credits      67.05     wall 611s
    LOC          38 production added, 44 test added, 6 files

`catches_defect` is verified, not asserted: the gate copies the agent's test
files onto the PRISTINE corpus, requires them to FAIL there, then requires them
to PASS against the agent's own fix. A test that passes on broken code, or fails
on its own fix, does not earn it.

This run fixed BOTH defects in the ticket. The earlier run fixed only the first.
That gives two measurements of the same arm on the same task, and they support
two different claims. Keep them apart:

| | deliverable | aimee | field | result |
|---|---|---|---|---|
| same work | defect 1 only, as all three controls | **46.25** | 63.66 - 75.82 | **27-39% cheaper** (Finding 21) |
| full work | both defects + two verified tests | **67.05** | 63.66 - 75.82 | **same price, only arm that finishes** |

So the honest framing is a CHOICE, not a single number:

  - Hold the deliverable fixed and aimee is significantly cheaper than every
    control -- 46-ish credits for what the others spend 64-76 on.
  - Hold the SPEND fixed and aimee completes the whole ticket and writes tests
    that provably catch the defects, while every control delivers half the
    ticket and no tests at all.

What the controls actually shipped on this ticket: baseline and
ponytail-instructions produced zero new code beyond the shared ordering fix;
ponytail-addon added one line. None of the three wrote a single test line. All
three still score hidden_ok=true, because the graded test only covers defect 1 --
which is why the TESTS column exists (Finding 18).

The ordering fix itself is worth noting for the writeup: src/posix/util.c scores
ZERO real code lines -- 7 raw '+' lines, of which 5 are comment and 2 are the
same statements relocated. The entire defect was ordering. aimee's own comment
names the mechanism: cwd may reach the directory through /proc/self/fd/<n>, and
when n equals the target fd, dup2 replaces the directory handle with the
credential memfd, so chdir fails and the child exits 127.

What this does NOT support:

  - One replicate. No confidence interval on either 46.25 or 67.05.
  - The control numbers are from the earlier run (valid -- controls do not
    exercise aimee -- but not simultaneous).
  - The controls' TESTS verdict is INFERRED from their recorded diffs (they
    changed zero test files, so no_test is certain) rather than produced by a
    gate run; their cells predate the gate and record tests_ok=N/A.
  - 46.25 and 67.05 come from different aimee builds. The delta is not a clean
    measurement of "second defect costs 21 credits" -- it is two runs that
    happened to deliver different scopes.

## Finding 23 — am_b84c9294aa: a wrong fix, and a gate that certified its test

Task 2 of the am_ corpus, aimee arm, same build as Finding 22.

    hidden_ok    FALSE  (1 graded test failed)
    tests_ok     true   reason: catches_defect      <-- WRONG, see below
    credits      54.01  wall 585s
    LOC          13 production, 13 test, 2 files

The ticket is a LEASE LEAK: 16 pool members held ~15 hours and never returned.
aimee did not fix the leak. It loosened the starvation DETECTOR, deleting the
waiters prerequisite:

    -  int starved = (g_size > 0 && stuck == g_size && stuck == live && waiters > 0);
    +  int starved = (g_size > 0 && stuck == g_size && stuck == live);

The self-heal action behind that flag is a PROCESS RESTART, and the prerequisite
is what stops it firing on a healthy pool. The upstream comment says so outright:
"a fully-leased pool with nobody queued is a busy kb, not a stuck one". aimee's
version restarts a busy kb underneath its users.

It then rewrote the test guarding that invariant --
test_busy_pool_is_not_treated_as_starved (asserts starved_calls == 0) became
test_stuck_pool_without_waiters_gives_up (asserts == 1). The graded suite still
contains the original, and that is the test that failed.

The reasoning was not stupid: a starved pool starves the HTTP worker pool, so
requests can block before ever reaching db2_pool_lease() to become waiters. That
is a real observation about the waiter signal. It is not a licence to delete the
guard. The roundtable ran TWICE and approved it.

### The gate defect this exposed

Red-green CANNOT distinguish "wrote a test that catches a defect" from "inverted
an existing assertion". A flipped assertion fails on pristine code and passes on
the changed code -- satisfying both halves -- so the gate awarded catches_defect
to a test whose only content was agreeing with the agent's own regression.

Fixed by an integrity check: any test entry point present in the PRISTINE tree
and absent from the agent's version yields

    tests_ok = false, reason = removed_existing_test:{file: [names]}

Re-scored from the stored artifacts, task 2 now returns exactly that, naming
test_busy_pool_is_not_treated_as_starved. Detection covers C, Python and JS/TS
test declarations. Deleting a test is a legitimate engineering act; it is simply
not evidence that a defect was caught, which is the only thing this column
claims.

Cells graded before this check (task 1, am_312e901904) are unaffected in
substance -- both of its test files were NEW, so nothing could have been removed
-- but any tests_ok=true produced before it should be re-derived, not trusted.

## Finding 24 — am_b84c9294aa: 0 of 4, and the fix was already in the tree

All four arms run on task 2, same corpus and build. Nobody passed.

| arm | hidden_ok | tests_ok | reason | credits | prod | test |
|---|---|---|---|---|---|---|
| aimee | false | false | removed_existing_test | 54.01 | 13 | 13 |
| baseline | false | false | does_not_catch_defect | 69.59 | 41 | 35 |
| ponytail-addon | false | false | removed_existing_test | 43.88 | 10 | 7 |
| ponytail-instructions | false | false | no_test | 84.64 | 5 | 0 |

aimee's row is RE-SCORED. Its cell was graded before the removed_existing_test
check existed and stored tests_ok=true/catches_defect; the summary carries both
values (tests_ok_as_graded, tests_ok_rescored) rather than an overwrite.

### Three different failure modes, three different gate verdicts

no_test              wrote 5 production lines and nothing else, at the highest
                     cost on the board
does_not_catch_defect wrote the most of anything (41/35) and its test PASSES on
                     the unfixed corpus
removed_existing_test deleted the guard test, twice, independently

This is the first task where the TESTS column separated four arms into distinct
diagnoses. It is measuring something.

### The wrong fix was not idiosyncratic

aimee AND ponytail-addon both deleted the waiters prerequisite from the
starvation detector and both removed the SAME guard,
test_busy_pool_is_not_treated_as_starved. Two independent arms, one with aimee
and one without. The trap is real: a starved pool starves the HTTP worker pool,
so requests block before they can queue as waiters, which makes the prerequisite
look like the bug.

### What the real fix is

The graded artifact is ONLY src/tests/test_db2_pool.c, and against pristine it
adds six lines:

    +   db2_pool_note_lease_site(a, "leaky_worker.c:123");
    ...
    +   assert(strstr(g_starved_reason, "leaky_worker.c:123") != NULL);

So the deliverable is: when the pool gives up, the fatal line must NAME THE
HOLDER, not just count members. The detector condition is untouched -- the
graded suite keeps test_busy_pool_is_not_treated_as_starved exactly as it was,
which is why every arm that loosened the condition failed.

The capability was ALREADY THERE, unused by that path:

    src/db2/db2_pool.h:103   void db2_pool_note_lease_site(void *conn, const char *site);
    src/db2/db2_pool.c:426   implementation
    src/db2/db2_init.c:534   already records g_lease_site on every lease
    src/tests/test_db2_pool.c:393  a pristine test already exercises it

The pool already knows who took every lease. The starvation message just never
included it, reporting counts only. This is Finding 15's pattern again: the
capability existed and nothing pointed at it.

Operationally that is the whole ticket. The condition is "unrecoverable
in-process" -- the only remedy is a restart -- so the single line emitted before
giving up is the only forensic evidence an operator ever gets. Without a holder
name you restart, the leak returns, and you are back in the same 15 hours.

### Why 0/4 is explainable by the ticket, not by four capability failures

The ticket is 265 bytes and was delivered correctly to every cell. It states the
DIAGNOSIS -- "it was a lease taken and never returned" -- and never states the
DELIVERABLE. Nothing in it mentions attribution, call sites, or which code
leaked; grepped, there is no such word. Read plainly it says "the detector
missed this", which is exactly what all four arms tried to fix.

Contrast task 1, whose ticket names both defects outright ("...and the Projects
view then disagreed with what had actually been cloned"). That one scored 1/4.

This is Finding 14's under-scoped-patch failure in its sharpest form: a solvable
ticket, a small fix, existing infrastructure, and a spec that points somewhere
else. A 0/4 here is an acceptable result for the study -- not every ticket must
be winnable -- but it should be reported as a SPEC gap, not as four independent
capability gaps.

## Finding 25 — am_270b3483d5: aimee alone fails, by over-validating

All four arms, one replicate, same harness and gate.

| arm | hidden_ok | tests_ok (re-scored) | credits | wall | prod | test |
|---|---|---|---|---|---|---|
| aimee | FALSE | false — changed_existing_assertions | 38.65 | 489s | 16 | 12 |
| baseline | true | true — catches_defect | 17.95 | 122s | 14 | 8 |
| ponytail-addon | true | true — catches_defect | 24.42 | 144s | 7 | 13 |
| ponytail-instructions | true | true — catches_defect | 17.51 | 103s | 7 | 7 |

aimee lost on every axis: the only failure, 4x the wall time, roughly 2x the
cost. This is the first UNAMBIGUOUS aimee-specific failure in the study --
unlike am_b84c9294aa, where all four arms failed a ticket that never stated its
deliverable.

### The mechanism: it did MORE than the spec allows

The graded test writes a file containing "{}" and requires READY. Upstream states
the intent outright: "Only a readable bundle is READY." Presence and
readability, deliberately NOT content validation.

baseline, which passes:

    if (stat(bundle_path, &st) != 0 || !S_ISREG(st.st_mode) ||
        access(bundle_path, R_OK) != 0)
       return SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE;

aimee, which fails:

    int bundle_ok = server_mgmt_jwks_trust_bundle_load(
        bundle_path, bundle, sizeof(bundle), &bundle_len) == 0 && bundle_len > 0;

A file containing "{}" is readable but is not a valid trust bundle, so aimee
returns NO_TRUST_BUNDLE where the test demands READY.

It was not a mistake of ignorance. The pristine header says the preflight "does
not claim that the mounted bundle or cached JWKS is valid; those are checked by
server_mgmt_jwks_cache_startup after DB1 opens". aimee REWROTE that comment to
justify the change ("must pass the same secure loader used by request
authorization") and added server_mgmt_jwks_cache.o to the test link line to pull
the loader in. It knowingly collapsed a documented layering boundary.

### The uncomfortable reading

aimee's tool profile on this cell: find_symbol x2, index x2,
preview_blast_radius x2. Better code search found
server_mgmt_jwks_trust_bundle_load, and having found it, aimee used it. The
richer context produced a MORE thorough fix that violates the layering the
codebase documents and the graded suite encodes.

That is a real failure mode and it is the mirror image of the usual complaint
about weaker agents. It is not "aimee could not find the code"; it is "aimee
found more code than the ticket wanted it to use". Worth stating plainly in the
article: capability is not automatically alignment with scope.

### Second gate blind spot, now closed

tests_ok was stored as catches_defect for aimee here, WRONG for the same reason
as Finding 23 but through a different hole. aimee inverted assertions inside the
existing test:

    -  assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_READY);
    +  assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE);

removed_existing_tests() could not fire: that file has ZERO named test functions,
it is a bare main(), so nothing was "removed".

Added weakened_existing_assertions(): any assertion present in the pristine test
and absent from the agent's version fails the column with
changed_existing_assertions. Adding new assertions is explicitly unaffected --
only disappearances are reported.

Re-scored across all four task 3 cells it flags aimee alone and leaves the three
passing controls clean, which is the discrimination that matters: it does not
punish touching a test file, only removing what that file used to prove.

## Finding 26 — the roundtable CAUSED the task 3 failure

Controlled A/B on am_270b3483d5. Same image (aimee-server:rt49), same corpus,
same ticket, same agent. The only variable is whether a review is served.

| | roundtable ON | roundtable OFF |
|---|---|---|
| hidden_ok | FALSE | **true** |
| tests_ok | true (re-scored false) | true — catches_defect |
| calls the secure loader | yes | **0** |
| credits | 67.59 | **33.30** |
| wall | 585s | **248s** |
| prod / test LOC | 22 / 20 | 13 / 13 |

With no reviewer aimee produced the minimal correct fix -- access(R_OK) + stat +
S_ISREG, materially the same three lines baseline used to pass -- for HALF the
credits and 2.4x faster. With the reviewer it moved trust-bundle CONTENT
validation into a preflight the codebase documents as presence-only, invented a
new enum state for it, and failed.

The control is sound. A live probe returned
{"status":"error","message":"roundtable review module is not attached to the
event bus","http_status":502}; the supervisor's module list omits roundtable; no
review was served during the cell; and the cell records exactly 2 errors against
its 2 roundtable_review calls. The module still prints one startup line naming
its bus address before exiting on the disable flag, which reads like it is
serving and is worth silencing.

### What this means

The review step is not a neutral safety net on this task. It is an AMPLIFIER of
scope creep. A qa seat handed a stricter validator approves it -- "more
validation" reads as better engineering to a reviewer with no stake in the
ticket's boundaries -- so the panel pushed aimee past the spec rather than
holding it to it.

That inverts the assumption behind Findings 20 and 23, which treated review
failures as the panel being too PERMISSIVE (rubber-stamping, certifying a
rewritten test). This is the opposite failure: the panel is too DEMANDING, and
its demands are unmoored from the request.

It also explains the regression the operator reported -- aimee used to lead this
task. The agent's own instinct is right; the review talks it out of it.

### What it does NOT show

  - ONE replicate per condition. The contrast is large (pass/fail, 2x cost) and
    the mechanism is identified in the diff, but it is n=1.
  - It does not exonerate the review generally. On am_312e901904 the same panel
    coincided with aimee being the only arm to fix both defects.
  - Requiring original_request (Finding 25's fix) did NOT help: the ON condition
    above already had it required at both boundaries. The panel had the ticket
    and still approved the over-validation, so the defect is in what the seat
    optimises for, not in what it was given.
  - aimee at 33.30 is still above the controls (17.51-24.42) for the same
    deliverable on this task.

### The open question

If a single qa seat reliably rewards thoroughness over scope, the fix is in the
seat, not the plumbing: a reviewer whose only question is "does this do what the
ticket asked, and nothing else" rather than a general quality reviewer. That is
testable the same way this was -- A/B one persona against another on a task with
a known-minimal correct answer.

## Finding 27 — the roundtable is a scope PUSH, and the ticket decides if that helps

The complement to Finding 26. Same A/B, on the opposite ticket shape.

am_312e901904 names TWO defects. am_270b3483d5 wants a three-line presence check.

| ticket | | roundtable ON | roundtable OFF |
|---|---|---|---|
| am_312e901904 | hidden_ok | true | true |
| (two defects) | **defect 2 fixed** | **YES** | **NO** |
| | test files | 2 (C + TS) | 1 (C only) |
| | credits | **67.05** | 74.25 |
| | wall | 611s | 376s |
| am_270b3483d5 | hidden_ok | **FALSE** | **true** |
| (minimal fix) | credits | 67.59 | **33.30** |
| | wall | 585s | **248s** |

On the two-defect ticket the reviewer was cheaper AND better: 67.05 credits for
the whole ticket against 74.25 for half of it. Without the panel aimee fixed
defect 1, wrote one test instead of two, and never touched
frontend/src/setup/ownerUrl -- the clone-reconciliation half -- at all.

On the minimal-fix ticket the same panel cost twice as much and FAILED.

### One mechanism, two outcomes

The panel does exactly one thing: it pushes for MORE. requirement_coverage
enumerates the asks and blocks on anything unaddressed, which is precisely right
when a ticket names two defects and the agent fixed one. The identical pressure,
applied to a ticket that wants presence-not-content, produces a stricter
validator that breaks a layering boundary the codebase documents.

So neither "keep it" nor "turn it off" is the answer. The prompt already
contains the counterweight -- "adding work the request did not ask for is drift
... even when it would be an improvement" -- and it is losing to a qa persona's
instinct that more validation is better engineering. The pressure needs to be
DIRECTIONAL: block on unmet requirements, and treat work with no antecedent in
the request as a finding with the same force.

### Caveats

  - ONE replicate per cell. Four cells total across the two tasks.
  - The LOC columns are NOT comparable between ON and OFF on am_312e901904. The
    ON cell's stored 82/0 predates the classifier fix (that test_added=0 is the
    bug Finding 22 records; re-derived it is 38/44), while the OFF cell's 12/28
    uses the corrected classifier. The defect-2 presence check -- does the patch
    touch ownerUrl at all -- is the reliable signal, not the line counts.
  - Both OFF cells still spent 2 roundtable_review calls that errored, so the
    OFF credits include the cost of trying and failing to get a review.

## Finding 28 — the full ON/OFF set: the panel helps, hurts, or is irrelevant, by ticket shape

Third and last cell of the A/B. am_b84c9294aa is the under-specified ticket
(Finding 24): it states a diagnosis, never a deliverable.

| | roundtable ON | roundtable OFF |
|---|---|---|
| hidden_ok | false | false |
| deletes the waiters guard | yes | **yes** |
| finds note_lease_site (the real fix) | no | **no** |
| tests_ok | true (re-scored false) | **false — removed_existing_test** |
| credits | 54.01 | 49.56 |
| wall | 585s | 345s |

aimee makes the SAME wrong fix with or without a reviewer, deleting the same
guard and missing the same existing helper. The panel is not the cause here; the
ticket is.

### The complete picture across three ticket shapes

| task | shape | ON | OFF | panel's effect |
|---|---|---|---|---|
| am_312e901904 | two defects | both fixed, 67.05 | one fixed, 74.25 | **helps** — cheaper and more complete |
| am_270b3483d5 | minimal fix | FAILS, 67.59 | passes, 33.30 | **hurts** — doubles cost, causes failure |
| am_b84c9294aa | under-specified | fails, 54.01 | fails, 49.56 | **irrelevant** — same wrong fix |

One mechanism -- push for more -- and three outcomes decided entirely by whether
the ticket actually wants more. That is a property of the REQUEST, which the
panel is given, so it is in principle detectable by the panel itself.

### Consequences for the article

The honest headline is not "review helps" or "review is overhead". It is that an
unconditional scope PUSH is right about a third of the time, wrong a third, and
irrelevant a third, on this corpus. A reviewer that could tell which ticket it is
holding would be strictly better than either always-on or always-off.

### Also confirmed here

removed_existing_test fired LIVE for the first time (previous instances were
re-scored from stored artifacts), correctly failing a cell whose only test change
was inverting an existing guard.

## Finding 29 — the integrity check needed an arbiter, and the graded suite is it

am_e4c4afa194 failed the TESTS column in ALL FOUR arms with
changed_existing_assertions, while all four PASSED the graded suite. A check that
fires on 100% of arms when every one of them satisfies the upstream contract is
measuring itself.

Every arm removed the identical assertion:

    assert(strcmp(sval(ins, "command"), "fact/ReleasePlan") == 0);

which pins the ledger command field to the RAW memory key. The ticket requires
exactly that to stop: "the bridge now emits a one-way FINGERPRINT ('mk:'+hash of
kind\x1fkey) in the command field, never the raw identity". Keeping that
assertion means not fixing the bug. All four arms were right and the gate called
all four wrong.

### The rule

Editing or deleting an existing test is only evidence of gaming when the GRADED
suite also fails. hidden_ok now arbitrates:

    hidden_ok false + test edited  -> fail (the agent changed the test to agree
                                      with a change upstream rejects)
    hidden_ok true  + test edited  -> pass, annotated
                                      ";edited_existing_tests_but_graded_ok"

Re-scored across every stored cell, this keeps both genuine catches --
am_b84c9294aa aimee and ponytail-addon, which deleted
test_busy_pool_is_not_treated_as_starved AND failed grading -- and clears all
four am_e4c4afa194 arms. No cell was re-run; the verdicts are re-derived from
stored artifacts.

### Why this kept happening

This is the third correction to the same column (Findings 23, 25, 29). The
pattern in all three: I wrote a rule from ONE cell's failure and shipped it as
general. Two were under-strict and let gaming through; this one was over-strict
and punished the correct fix. The check only became trustworthy once it had an
independent authority to defer to rather than judging on its own.

Worth stating in the article: a test-quality gate is itself a measuring
instrument, and it needs calibrating against known-good and known-bad cells
before its output means anything. Every number this column produced before this
finding was re-derived, not re-run -- which is only possible because the cells
keep their full artifacts.

## Finding 30 — tasks 4-6, four arms, and the review on/off pair

First tasks graded by the corrected gate from the start: both integrity checks
live, arbitrated by the graded suite (Finding 29), and the LOC classifier fixed.
Nothing here needed re-scoring.

### am_1f0f1ab528 — 4/4, the cleanest cost comparison in the corpus

| arm | hidden_ok | tests_ok | credits | wall | prod | test |
|---|---|---|---|---|---|---|
| aimee | true | catches_defect | 44.66 | 464s | 27 | 36 |
| baseline | true | catches_defect | 30.51 | 315s | 40 | 40 |
| ponytail-addon | true | catches_defect | **26.61** | 162s | 19 | 30 |
| ponytail-instructions | true | catches_defect | 32.79 | 192s | 25 | 25 |

Every arm solves it and every arm writes a test that genuinely catches the
defect. Capability does not separate them, so this is a pure cost row -- and
aimee is the most expensive, at 1.68x the cheapest arm.

### am_e4c4afa194 — 4/4

| arm | hidden_ok | tests_ok | credits | wall |
|---|---|---|---|---|
| aimee | true | catches_defect (edited, graded ok) | 37.05 | 487s |
| baseline | true | catches_defect | 28.46 | 147s |
| ponytail-addon | true | catches_defect | **22.95** | 141s |
| ponytail-instructions | true | catches_defect | 26.89 | 152s |

The tests_ok=false this task first reported for ALL FOUR arms was the gate's own
bug; see Finding 29.

### am_1e7cb3da16 — 0/4, an unguessable identifier

| arm | hidden_ok | tests_ok | credits | chosen pause_reason |
|---|---|---|---|---|
| aimee | false | catches_defect | 43.13 | children_pending |
| baseline | false | catches_defect | 43.79 | (not slices_running) |
| ponytail-addon | false | changed_existing_assertions | 39.48 | (not slices_running) |
| ponytail-instructions | false | changed_existing_assertions | 38.18 | (not slices_running) |

The graded test asserts strcmp(wi.pause_reason, "slices_running") == 0 in FOUR
places. The ticket says pending_human is wrong and never names the replacement.
Zero of four arms guessed it. aimee and baseline still earned catches_defect --
they implemented the right BEHAVIOUR (a self-resolving wait the sweep re-drives)
under a different name.

### The review on/off pair, all six tasks measured

| task | ON | OFF | effect |
|---|---|---|---|
| am_312e901904 | both defects, 67.05 | one defect, 74.25 | helps |
| am_270b3483d5 | FAILS, 67.59 | passes, 33.30 | hurts |
| am_b84c9294aa | fails, 54.01 | fails, 49.56 | irrelevant |
| am_1f0f1ab528 | passes, 44.66 | passes, 37.74 | neutral, cheaper off |
| am_e4c4afa194 | passes, 37.05 | passes, 28.60 | neutral, cheaper off |
| am_1e7cb3da16 | fails, 43.13 | fails, 44.86 | irrelevant |

Review changed the OUTCOME on two of six tasks -- helping once, hurting once. On
the four where the outcome was unchanged it cost 6-13 credits per task for
nothing. That is the case for making it optional per run, which is how aimee
ships it.

### The corpus caveat that has to be published

Two of six tickets were unwinnable as written: am_b84c9294aa stated a diagnosis
and never its deliverable, am_1e7cb3da16 required an identifier it never named.
Both have since been repaired to the level of detail am_e4c4afa194 already used,
and cells before and after that repair ARE NOT COMPARABLE. Pass rates quoted
from the pre-repair corpus understate every arm equally, but they understate.

## Finding 31 — repairing two tickets turned 0/8 into 8/10, and made everyone cheaper

Findings 24 and 30 identified two tickets as unwinnable as written. Both were
repaired with ONE sentence naming the deliverable the graded test asserts, at the
level of detail am_e4c4afa194's ticket already used. Neither addition hands over
an implementation.

am_b84c9294aa gained: "The pool already records a lease site for every member,
but the line it emits before giving up reports only counts. It must name the
holders, so the operator restarting the process knows which code leaked."

am_1e7cb3da16 gained: "Park the parent under a distinct self-resolving pause
reason, `slices_running`, which the autonomy sweep re-drives once every child has
merged; `pending_human` stays reserved for genuine human gates." The identifier
had to be named because the graded test does strcmp on it in four places.

### am_b84c9294aa: 0/4 -> 5/5

| config | before | after |
|---|---|---|
| aimee (review on) | fail, 54.01 | **pass, 22.12** |
| aimee (review off) | fail, 49.56 | **pass, 29.53** |
| baseline | fail, 69.59 | pass, 19.04 |
| ponytail-instructions | fail, 84.64 | pass, 19.83 |
| ponytail-addon | fail, 43.88 | pass, 25.20 |

Every arm got CHEAPER as well as correct -- ponytail-instructions by 4.3x. A
ticket that makes agents hunt for the deliverable costs real money: they explore
hard, converge on a plausible wrong fix, and fail anyway.

Review-on is cheaper here than review-off (22.12 vs 29.53) and writes half the
production lines (27 vs 54): the reviewer pushed toward the focused attribution
fix instead of a broader rewrite.

### am_1e7cb3da16: 0/4 -> 3/5

| config | after |
|---|---|
| aimee (review on) | **pass, 48.50** |
| aimee (review off) | **pass, 43.09** |
| ponytail-addon | pass, 39.72 |
| baseline | FAIL, 38.72 |
| ponytail-instructions | FAIL, 22.48 |

All four arms now emit slices_running, so the naming lottery is gone and the task
discriminates on the AUTO-RESUME behaviour it was always meant to test. Two arms
still get that wrong. aimee passes in both configurations.

### Corrections to the record

Two of the three tasks previously reported as aimee failures were ticket defects:

  - am_b84c9294aa was reported as "aimee deleted a safety guard". True, but so
    did ponytail-addon, independently, and the ticket never stated its
    deliverable. Now 5/5.
  - am_1e7cb3da16 was reported as 0/4 with no arm passing. It was an unguessable
    identifier. Now 3/5.
  - am_270b3483d5 REMAINS a genuine aimee failure: over-validation with review
    on, passing with review off (Finding 26). That one stands.

### The methodological point

A benchmark ticket has to state the deliverable the grader checks, or it measures
guessing. The tell is cheap to compute and should be checked BEFORE running a
corpus: if the graded test asserts something no reading of the ticket entails --
a behaviour never mentioned, an identifier never named -- the task is unwinnable
and the 0/N result says nothing about any arm.

Cells before and after the repair are not comparable, and the pre-repair cells
are archived under pre-ticket-repair-<task>/ rather than discarded.

## Finding 32 — am_842ff35656 is a THIRD spec gap (proposed repair, NOT applied)

Flagged for review, not acted on: ticket edits change what the corpus measures
and should not be made unattended.

aimee fails this task in the review-off pass (hidden_ok false, tests_ok
does_not_catch_defect) despite writing 68 lines of test -- the most test code in
any cell of the sweep, none of which exercises the graded defect.

### The mismatch

The graded test adds test_large_roundtable_payload_within_limit to
src/tests/test_server_dispatch.c. It builds a roundtable.review message of
LIMIT_DEFAULT + 64 bytes, pushes it through dispatch_json, and requires
status ok. So the deliverable is SERVER-SIDE: roundtable.review must join the
per-route exemption table.

That table already exists, at src/server/server.c method_size_limit():

    {"memory.", LIMIT_MEMORY},
    {"tool.", LIMIT_TOOL},
    {"delegate", LIMIT_DELEGATE},
    {"mcp.call", LIMIT_DELEGATE},
    {"chat.", LIMIT_CHAT},
    ...

roundtable.review is absent, so it falls through to LIMIT_DEFAULT and is
rejected. The fix is one row.

The ticket points somewhere else:

    "The CLI capped what it would marshal from a file or from standard input at
     a fixed 2 MB"

Word counts across the whole ticket: CLI 1, transport 1, marshal 1, dispatch 0,
server 0. aimee changed src/cli_v1_routes.c and its test -- exactly the layer
named. It fixed what it was told to fix.

### Proposed repair

Append to the ticket, matching the level of detail used by the two tickets
repaired in Finding 31:

    "The same conflation exists at the server's per-route payload table, where
     roundtable.review is missing from the exemptions delegate and mcp.call
     already have, so a large artifact is refused before dispatch."

That names the layer and the symptom without handing over the row.

### CONFIRMED at 0/5 (all arms now measured)

| arm | verdict | credits | prod / test |
|---|---|---|---|
| ponytail-instructions | FAIL does_not_catch_defect | 26.60 | 13 / 24 |
| ponytail-addon | FAIL does_not_catch_defect | 34.92 | 2 / 42 |
| aimee (review off) | FAIL does_not_catch_defect | 36.56 | 11 / 68 |
| baseline | FAIL does_not_catch_defect | 43.45 | 16 / 49 |
| aimee (review on) | FAIL does_not_catch_defect | 66.74 | 30 / 51 |

234 lines of test across five arms and not one catches the defect, because every
arm tested the CLI marshalling layer the ticket names while the grader checks the
server dispatch table. Five independent agents across four scaffolds all landed
on the same layer and all were wrong by the grader. The ticket steered every one
of them, which is what distinguishes this from a task that is merely hard
(compare Finding 34: am_67e9b0449a, 3/5 pass).

### Why this matters beyond one task

Three of fourteen tickets have now shown the same defect: the graded test
asserts something no reading of the ticket entails. am_b84c9294aa (a behaviour
never mentioned), am_1e7cb3da16 (an identifier never named), am_842ff35656 (the
wrong layer named). Each produced a failure that looks like a capability gap and
is not.

The check is static and cheap: for every task, read the graded diff and ask
whether the ticket entails it. That should run BEFORE a corpus is used, not
after three sweeps of misattributed failures.

Not yet checked the same way: am_67e9b0449a, the other review-off failure.

## Finding 33 — am_67e9b0449a: the ticket names neither layer (proposed repair, NOT applied)

The other review-off failure, checked the same way as Finding 32. Same family,
weaker case: the ticket does not name the WRONG layer, it names NEITHER.

The graded test exercises kb_management_action_body_parse, declared in
src/kb/kb_management_action.h:98 and implemented in src/kb/kb_management_action.c
-- the KB layer. It feeds three inputs: an escaped NUL in the action value, an
escaped NUL in the agent value, and a LITERAL backslash-u0000 that must be
treated differently from the escape.

aimee changed src/server/server_mgmt_endpoint.c and its test -- the server layer.

Ticket word counts, whole text: management 1, parser 1, request 1, kb 0,
server 0. "A management request" is genuinely ambiguous: both surfaces are
management, and nothing in the ticket selects between them. Unlike am_842ff35656,
where the ticket actively named the CLI and the grader checked the server, this
one simply never says.

### Proposed repair

    "The check belongs in the KB's management action body parser
     (kb_management_action_body_parse), where the decoded value is produced,
     rather than at an individual endpoint."

Names the layer and the reason without giving the implementation.

### Where the corpus stands

Four of fourteen tickets now show a gap between what the ticket entails and what
the graded test asserts:

| task | gap |
|---|---|
| am_b84c9294aa | deliverable never stated | REPAIRED, 0/4 -> 5/5 |
| am_1e7cb3da16 | identifier never named | REPAIRED, 0/4 -> 3/5 |
| am_842ff35656 | wrong layer named | proposed |
| am_67e9b0449a | neither layer named | proposed |

The two repaired cases both went from total failure to majority pass on one
sentence, which is the reason to take the other two seriously rather than
recording them as capability gaps.

Note what this does NOT license: the aimee failure on am_270b3483d5 was checked
the same way and is real -- the ticket entails the presence check, the graded
test asserts the presence check, and aimee did content validation instead.

## Finding 34 — Finding 33 WITHDRAWN: am_67e9b0449a is hard, not unwinnable

Finding 33 proposed repairing am_67e9b0449a's ticket because aimee (review off)
fixed the server layer while the graded test exercises the KB parser, and the
ticket names neither. That was argued from ONE failing cell. With all five arms
now measured it does not hold:

| arm | verdict | credits |
|---|---|---|
| aimee (review on) | pass | **24.34** |
| ponytail-instructions | pass | 31.75 |
| ponytail-addon | pass | 34.87 |
| aimee (review off) | FAIL | 28.98 |
| baseline | FAIL | 21.34 |

Three of five arms find the right layer, so the ticket is navigable. Two do not,
which is a task discriminating on difficulty -- exactly what a benchmark ticket
should do. No repair is warranted and the proposal is withdrawn.

Note baseline fails it as well, so the review-off failure is not aimee-specific.

The contrast with the genuine gap is now sharp and gives the test a threshold:

    am_842ff35656   0 of 5 arms pass   ticket names the CLI, grader checks the
                                       server dispatch table   -> SPEC GAP
    am_67e9b0449a   3 of 5 arms pass   ticket names neither layer, most arms
                                       still find it           -> HARD TASK

One failing arm is not evidence a ticket is broken. A ticket no arm can pass,
where the graded diff touches something the ticket never mentions, is.

Also worth recording: aimee with review ON is the CHEAPEST passing arm here, and
review is what redirected it from server_mgmt_endpoint.c to
kb_management_action.c. This is the clearest case in the study of the panel
earning its cost -- Finding 26 showed it causing a failure, this shows it
preventing one, for less money than the arms that needed no help.

## Caveats for anything published from this

- 6 of 8 tasks, **one replicate**, no confidence intervals. Per-task spread is
  2.61× to 0.84×.
- Finding 21's LOC figures (27/19) predate the classifier fix and were counted
  with `loc_real.py`; Finding 22's (38/44) are raw diff lines from the harness
  after that fix. **Do not compare them directly** — one excludes comments,
  includes and moved code, the other does not.
- Control arms are from an earlier run than the aimee arm. Valid because controls
  do not exercise aimee, but it is not a simultaneous comparison.
- USD rests on the published Sol card; credits and all ratios are card-independent.
- Finding 7's fixes are **built and unit-tested, not yet benchmarked**.
