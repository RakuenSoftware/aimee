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

