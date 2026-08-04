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

