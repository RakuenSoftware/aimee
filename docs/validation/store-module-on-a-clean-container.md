# The store module on a machine that never built it

Record of running aimee-server, aimee-kb and the module fleet on a container
that received only binaries, against a PostgreSQL that already existed.

Reproduce with `scripts/e2e-252-full.sh` inside the container, after
`scripts/e2e-252-dbsetup.sh` and `scripts/e2e-252-install-modules.sh`.

## What was used

- **Host**: Proxmox at 192.168.1.252, container CT 8150.
- **PostgreSQL**: 17.11, pgvector 0.8.0.
- **Build**: `pre-merge-safety-2698-g553e859cbe`, built locally and carried in.
  Nothing was compiled on the host or in the container.
- **Databases**: `aimee_e2e_store` and `aimee_e2e_kb`, both created **UTF8**
  with `TEMPLATE template0`. The container's cluster initdb's SQL_ASCII, where
  `char_length` and `octet_length` are the same function, so a
  byte-versus-character constraint passes there whether or not it is correct.
  Production runs `initdb --encoding=UTF8`; matching it is what makes the run
  mean anything.

## Result

### `scripts/e2e-252-full.sh` -- 10 checks

**Eight ran. Two never executed**, and the summary line below does not say so:
`PASS=8 FAIL=0` reads as a complete passing run, and it is a partial one. The
two unexecuted checks. That the store module created its schema, and that
migrations were recorded, sit inside the branch the BLOCKED row describes, so
the module never reached them.

That is the under-claiming direction: a check that never ran leaves no trace,
and a smaller passing run is the shape nobody investigates. Recorded here
because `scripts/check-validation-record.py` compares this count against the
call sites in the script, and the count it compares against is 10.

    PASS  aimee-kb is running
    PASS  aimee-kb tables in DB2 (217)
    PASS  pgvector is installed in DB2
    PASS  aimee-server is running
    PASS  the module bus socket exists
    PASS  the daemon created no SQLite file
    PASS  the daemon holds no .db descriptor
    PASS  the supervisor started modules (11)
    BLOCKED  the store module attached and stopped at its one missing dependency
    PASS=8 FAIL=0        (2 of 10 checks not reached; see above)

**SUPERSEDED. The gap is closed and the run below was repeated in full; see
"Run two" at the end of this document.**

## The one gap, isolated

The store module attaches, builds its store client, and stops here:

    store: schema: read the applied schema version:
    aimee: the postgres module is not answering

It holds principal ref 30 serving and ref 68 outbound, and asks for kind 11266,
the postgres module's SQL stage. Nothing serves that stage in this tree, so
there is no answer. Everything before that point works; the module is complete
and its dependency is not present.

That gap closes when the postgres module's SQL stage lands. Nothing else is
outstanding, and the registry entry for the outbound client cannot be declared
until then either: the contract validator refuses a client requesting a kind
nobody serves.

## Four wrong diagnoses on the way, because each was a real trap

- **The KB bound the server's module socket.** `AIMEE_MODULE_BUS_SOCKET` was
  exported into the shared environment and aimee-kb starts first, so it took the
  path aimee-server then failed to bind, reporting `module endpoint failed` on a
  socket something else owned. Both daemons read that variable; only one may be
  given it.
- **One unusable grant refuses the whole directory.** The grants pin absolute
  executables and only `aimee-module-aimee` was installed, so
  `governance.grant` was unresolvable and the daemon refused to start entirely,
not a degraded module, a dead server. The module runtime is a multicall binary,
  so the fix is to install it under every name the grants name.
- **Grant policy is read once, at startup.** A grant added while the daemon was
  already running was invisible, and the module was denied for a reason that had
  nothing to do with the grant's contents.
- **A rebuilt binary that was never redeployed.** The outbound ref was corrected
  from 67 to 68 in the source and the artifact was rebuilt, but the container
  still ran the old one, so the grant said 68 and the process attached as 67.
  The prose had said 68 for hours. A comment is not what attaches to a bus.

## What the run says about SQLite

The daemon creates no `.db` file and holds no descriptor on one, checked from
`/proc/<pid>/fd` so it is a property of the process rather than of the code that
was read. `aimee-kb` carries no libsqlite3 at all. `aimee-server` still links it
for the audit WORM ledger, PKI and kb-synthesis, which are unrelated to the
store; `make -C src check-linking` asserts each binary's boundary.

## What the retirement took with it, and what came back

Ninety-three C test files were deleted alongside the C store module. A scan for
`db1_|sqlite3|db1/` classified 93 of 93 as store-coupled, which reads as every
deletion being justified, and is the same defect this document keeps recording:
a true statement that does not support the conclusion drawn from it.

Coupling is not subject. Ranked by assertions per store-referencing line, the
top of the list is not made of store tests:

    test_memory_advanced.c      513 assertions    5 store lines
    test_mcp_git.c              675 assertions    7 store lines
    test_guardrails.c           673 assertions   11 store lines
    test_agent.c                826 assertions   14 store lines
    test_http_retry.c           105 assertions    2 store lines

`test_http_retry.c` was the extreme case, and its two matches were:

    * db1_conn; this test binary doesn't link db1. Stub it to NULL so recording
    void *db1_conn(void)

**A comment stating the file does not link the store, and the stub that made it
true.** It was counted as coupled on the strength of prose denying the coupling.

The coupling was real, but it lived in the link line rather than the source:
`modules/db1/interaction_events.o`, pulled in for one symbol `failover.o` calls.
That object was made inert by stubbing `db1_conn` to NULL so the real recorder
had no connection to write through, a trick that only works while the store is
in-process. It is a bus client now, reaching a separate process, so there is no
handle to withhold.

Restored, with the dependency cut at the call instead
(`tests/support/interaction_events_stub.c`): 105 assertions about retry backoff,
overflow-safe clamping, stall caps, parallel cancel and body identity across
attempts, none of it about storage, and none of it with another home. It passes.

`unit-test-wfe-blocks` failed the same way in the other direction. The target
survived and its link line did not. It had satisfied `wfe_blocks.o`'s work-item
references with `db1_init.o`, `db_schema.o` and `wfe_store.o`, under a comment
reading "no engine/db1; the schema + externalization tests are pure". Pure, and
linked against a real database anyway, because a link line answers to the linker
rather than to the test. It now links `tests/support/wfe_store_stub.c`, whose
five functions **return failure rather than success**, so a future test that
starts depending on them fails and comes here instead of asserting against a
zeroed struct it believes it read.

The other four files above have real store calls and are not reinstated here:
whether their subject moved to Go is a judgment about each one, not a scan
result, and this document is not the place to make it. The four are recorded so the question is
answerable rather than invisible.

## The gates that were red, and why

`make lint` ran five failing targets at the end of this work. Four are now green:

- **bus-blast-radius-check**: the DB1 client became a bus client, so the dev rig
  `../write-tier-enforce-live` inherited the event-bus archive through
  `$(DB1_CLIENT_OBJS)`. The check matched the TEXT of each hit line against
  `$(SERVER):`/`$(KB):`, which cannot work for a link line whose prerequisites
  wrap onto a continuation line carrying no target name. It now attributes each
  reference to the target that OWNS it, which is stronger in both directions: a
  continuation line is read correctly, and a new target cannot be admitted by
  happening to contain `$(SERVER):` somewhere. A permitted owner that stops
  referencing the archive now fails as a stale allowance, and a scan finding
  zero references fails rather than reporting a clean boundary.
- **db2-declaration-ledger-check** and **config-encapsulation-check**, regenerated;
  both were recording debt against files the retirement removed.
- **aimee-home-check**: eleven files whose aligned trailing comments shifted when
  the include paths changed from `modules/db1/` to `db1_client/`. Reformatted.

**docs-gen-check remains red and cannot go green here.** It regenerates
`docs/gen`, then runs `git diff --quiet` against it. The generated content on
disk is correct; the check is measuring that the tree is uncommitted, which it
is, deliberately. It clears on commit and not before.

## Three guards that read prose as code

A peer session found that its validation-record guard matched check names
against the whole document, so output it had quoted verbatim in the prose
satisfied the guard it had written. That is the same shape as the `db1_conn`
comment above, and it is a shape, not an incident, so the validators added by
this work were checked for it rather than assumed clean. Three had it.

**check-log-prefix-ownership** matched a quoted prefix anywhere in a Go file. A
comment reading `must never say "postgres: "` is a quoted prefix, so the check
reported the module as emitting the prefix its comment forbids. Demonstrated by
planting exactly that comment. This is the corrosive direction: the cheapest way
to pass is to delete the sentence explaining the rule, so the guard trains
people to remove rationale.

**check-memory-store-degradation** was the false-clean direction and the worse
of the two. It decided a file degraded gracefully if `AIMEE_DB2_DISABLED`
appeared anywhere in it, and that it reported failures if `LOG_WARN(` did. A
planted file reaching the store with neither, carrying only a TODO comment
promising both, came back clean. The guard read intent as implementation.

Both now strip comments first, via `scripts/source_text.py`. Quotes are tracked
rather than ignored: the `//` inside `"http://host/"` does not open a comment,
and blanking from there would truncate the literal these checks exist to read.
Mutation-verified in both directions, planted prose no longer fires, planted
code still does, and a literal containing `//` is still read past.

**check-validation-record was inert and said `ok`.** Its heading pattern
required `scripts/validation/*.sh`; the live probe is `scripts/e2e-252-full.sh`,
so nothing matched. It then took the `not checked and not problems` path and
returned 0. Its own docstring names this failure for an empty glob and guards
that case; the same condition one level in, records found, read, and none
asserting anything, returned success. Marking the previous record HISTORICAL
was enough to reach it, which is an ordinary edit nobody would expect to disarm
a gate.

It now accepts any `scripts/` path, and comparing nothing FAILS with the heading
form it wants. Which immediately caught the record above: the probe defines 10
checks and the run executed 8, because the two inside the branch the BLOCKED row
describes were never reached. `PASS=8 FAIL=0` had reported a partial run in the
shape of a complete one.

## Two more, from the same sweep

**Zero on both sides is not agreement.** `check-validation-record` compares a
record's claimed count against the call sites in the probe it names. A probe
that defines its `ck` helper and never calls it, paired with a record claiming
`0 checks`, agrees perfectly, and the equality is between two absences. Worse
than passing quietly: it increments the comparison counter, so an inert pair
made the summary report MORE comparisons than the tree supported. Planted
exactly that pair to watch it report `2 count(s) match`. A zero on either side
now fails.

**A refusal test that refuses nothing passes trivially.** The variadic-bounds
test named the five operations it knew about, which is an enumerated gate with
the failure mode of every enumerated gate: an operation added tomorrow is
outside the hand-written map and nothing says so. It now derives its set by
walking `All()` for `Args < 0`, so a new variadic operation is covered by
existing code and the only way out of the test is to stop being variadic.

The derivation found the same five, which is the useful part, it agrees with
the hand-list rather than replacing it with a guess. And an empty set now fails
loudly, because the one thing a refusal test cannot establish by observing
refusals is whether any were requested.

Both mutation-verified: emptying the walk fails with a message naming the two
possible causes, and removing `windowTerms`' own bound still produces
`index out of range [0] with length 0` on a 0-cell frame.

A caution worth recording alongside these, from the peer session running the
same sweep: **not every collection should be guarded against being empty.**
Emptying their arity TABLE makes their test stricter, not weaker. Every shape
becomes unknown and any success fails. The reflex after a sweep like this is to
add an emptiness guard everywhere, and half the time that guards the safe
direction. The question is which way the collection fails, not whether it can be
empty.

## A failure whose obvious repair is wrong

`check-validation-record` fails loudly when a record's claimed count disagrees
with the probe it names. The repair that failure invites is to edit the number
in the markdown until it matches. One character, and the gate goes green.

That repair makes the record assert a count no run produced: a claim about a
container that no longer exists, which is the precise thing the check was
written to prevent, arriving through the check's own failure message.

So the message now names the wrong repair rather than only the disagreement.
Re-run the probe and record what the new run reported, or mark the record
historical. Do not reconcile the number.

This is a third category alongside the two the sweep above turned up. A silent
green that means nothing is the worst; a loud failure is fine; **a loud failure
whose obvious fix over-claims sits between them**, and it earns a comment even
though the failure itself is perfectly clear. Guarding it is not possible. The
edit is legitimate when a run did change, so the defence is that the
person making it is told what they are asserting.

## Where the hazard was written down

The peer session found its wrong-repair warning sitting in a COMMENT, the place
a guard's author looks, while the person who meets the failure reads the
MESSAGE, which said nothing about it. The warning had been filed somewhere
nobody standing in the trap can see it.

Two guards here had the same fault, and both invited the same wrong repair:
adding a name to an allowlist.

**bus-blast-radius** reported which target owned the stray reference and stopped
there. The one-line repair is to add that target to `BUS_LIB_OWNERS`, after
which the check goes green having recorded the widening as permission for it.
The reasoning against that was in the comment above the list. The failure now
says it: ask why the target needs the bus at all, because it usually links an
object that became a bus client, and the answer is to stub the call or drop the
object.

**check-log-prefix-ownership** named the harm and no remedy. The obvious repair
is to add the module name to `AMBIGUOUS`, which silences the finding and leaves
the operator being sent to the wrong process, the entire harm, preserved. The
failure now says to fix the string, and states what `AMBIGUOUS` is actually for.

Both were then read the way someone meeting them at speed reads them, by making
them fire, rather than the way their author reads them. That is what caught the
second problem: the log-prefix text was correct and arrived as one unwrapped
wall. Correct and unreadable is a failure of the same instrument.

This is the instrument the third category leaves. Ranging, self-bounding and
caller-bounded are properties of code. A loud failure whose obvious repair
over-claims is a property of the human path out of a failure, and the only tool
that reaches it is the sentence that gets printed.

## Two repairs offered as equivalent

The peer session found the sharper form of the allowlist hazard: a message that
offers two repairs *as alternatives* when one is a YAML edit in a file you must
first find and the other is a single line in the file already on screen. Nobody
at speed picks the first, so offering them as equals is choosing the cheap one
on the reader's behalf.

`check-validation-record` had it twice, and the worse instance is the one where
the cheap repair is **exactly how this check went inert in the first place**.

Its "compared nothing" branch said: give the live record a counted heading, *or
mark it historical too*. Learning N means running the probe. Marking a record
historical is one line, and it makes that record permanently unchecked, which
is the state the branch exists to report. The message recommended, as a co-equal
alternative, the act whose consequence it was written to catch. That is recorded
three sections up as the thing that disarmed the gate, in the docstring of the
same file.

The mismatch branch had the same pairing around a third option.

Both now rank the repairs and say what the cheap one *claims* rather than what
it costs. Marking a record historical asserts that it no longer describes the
current program and may not be cited as evidence for anything current, true
after a rewrite, not true because a count moved. Verified by firing both
branches and reading the output cold.

The general form, for anything with an exemption list or an escape hatch: **an
alternative offered in the same breath as the repair is not an alternative, it
is the default.** If one path is cheaper and weaker, saying so is the only thing
that stops it being the one taken.

## The instrument, and its test

Ranging, self-bounding and caller-bounded are properties of code, and each can
be established by making the code fail. The wrong-repair hazard is a property of
the human path out of a failure; there is no fire available, because the edit
that over-claims is legitimate when the underlying run did change.

So both sessions filed the warning in a comment without noticing. A comment is
the natural home for a fact about the code. This is a fact about the person who
arrives when the code fails, and it belongs where they
are standing.

It does have a test, just not an automated one: fire the guard, and read what it
prints as someone who has never seen it. Firing is how you get the text; being
unfamiliar with it is the part you have to fake. That step caught a second fault
here and in the peer tree independently, in both cases a remedy that was
correct and arrived as one unwrapped wall, written thirty seconds earlier and
therefore invisible to its author.

## Re-measured by exit code

Most readings in this work were taken by grepping a command's output. That is a
second-hand instrument: it reports what a program SAID, and a program that dies
early says nothing and greps clean. Worse, `cmd | filter; echo $?` reports the
FILTER's status, `head` essentially always succeeds, so a failing build reads
as `rc=0`. That mistake was made here, live, while checking one of these very
guards, and caught only because the number disagreed with the message above it.

So every gate was re-run and read by exit code alone:

    go build ./...                     0
    go vet ./...                       0
    go test ./...                      0
    go test -race ./modules/aimee/...  0
    go test -race ./bus/...            0
    make -C src unit-tests             0
    11 validators, individually        0
    make -C src lint                   2   <- docs-gen-check, and only that
    make -C src docs-gen-check         2

The answers stand. They stand because they were checked, not because the earlier
readings meant anything.

**The shape is not in any shipped script.** A first scan flagged 24 sites and
every one was fine, because it looked for the wrong thing, `$?` after a
pipeline, which matches the CORRECT idiom:

    echo "$OUT" | grep -q '"db2_ok":true'; check $? "..."

There the assertion is LAST, so `$?` is grep's status and grep's status is the
question. The defect is the inverse: the thing under test first, a pass-through
formatter last. Rescanning for that. A pipeline ending in
`head|tail|cat|tr|tee|...` whose status is consumed, found three candidates,
all correct, and the two subtle ones were settled by demonstration rather than
by reasoning about expansion order:

    f "$?" "$(exit 7; echo sub)"    ->  arg1 is still the OLD status

`$?` is expanded before the later command substitution runs, so
`check $? "..." "$(echo "$out" | tail -1)"` reads the assertion's status and not
`tail`'s. The CI hits were all `make --version | head -1 || true`, diagnostics
with the status explicitly discarded.

`scripts/e2e-252-full.sh` (the probe this record cites) was checked separately
because its verdict is the thing being claimed. `ck` runs its command directly,
with no pipe. `sqlv` DOES pipe psql into `tr`, so its exit status is `tr`'s and
always zero; what `ck_gt` consumes is its OUTPUT, and a database refusing every
query produces none. Demonstrated rather than argued: with a failing query the
helper still exits 0 and the check still reports `FAIL (got 0, want > 100)`.

Both sessions made the pipe mistake in their hands rather than in their code,
on the same night, and neither found it by reading.

## A scanner wrong about everything, and where scope is load-bearing

The corrected pipe scan produced a category none of the shapes above covers. A
scanner that flags 24 sites and is wrong about all of them is **the same object
as a guard that passes having checked nothing**. It just fails loudly instead
of quietly. Loud-and-wrong and silent-and-wrong are one defect in different
clothes, and the loud one is more expensive: it gets switched off rather than
answered.

Which makes scope load-bearing wherever a guard's rule is true only locally. Two
guards here were measured against a one-word widening of their path, and they
gave **opposite answers**:

**check-memory-store-degradation. Do not widen.** Its rule is that a file
reaching db2 must branch on `AIMEE_DB2_DISABLED` or report. That is true for
memory recall, where an empty result is indistinguishable from a genuine absence
and silence is the harm. It is not general: most code reaching db2 should fail
hard, and for it a fork would be the defect. Measured: 442 files outside
`src/modules/memory` call `db2_*`, and **406 of them would be reported SILENT**.
The distinction is invisible from inside the directory, because within it a db2
call with no fork genuinely is a defect every time, and that coincidence is
exactly what makes widening look safe.

**check-log-prefix-ownership. Do not widen, for the opposite reason.** Measured
across 47 further files outside `server-go/modules`: it would fire **zero**
times. That is not a licence. Zero hits says the rule does not currently
misfire; it says nothing about whether the premise holds, and outside `modules/`
it does not, `server-go/bus` is a package, not a module with a principal ref,
so there is no owner for a prefix to disagree with. Extending a rule into a
directory where its premise is absent buys coverage that cannot mean anything.

Both conclusions are now written at the path constant, where someone reaching to
widen it will meet them, rather than in a commit message or a record. The two
answers being opposite is the point: **the question has to be asked per guard,
and neither answer generalises.** Reaching for either by pattern is how the 24
happened.

## Coverage stated, and one claim re-derived rather than repeated

Two last passes, prompted by a peer session asking the same questions of its own
guards and getting different answers.

**The record guard vouched for one file and said `ok` beside eighty-eight.**
Its summary read `ok (1 count matches, 1 historical)`. There are 88 records in
`docs/validation/`; it compares exactly one. The other 86 assert no count, which
is not a lie and not a defect (a record is free to be prose) but a summary
that omits how few it could reach **grows more reassuring as the directory
grows**, which is the wrong direction for a number to move.

Fixed by stating the denominator, not by making 88 records carry counts. That
would be the over-claiming repair at scale: a check total recorded against 87
runs nobody re-ran. It now says:

    ok (1 of 88 record(s) assert a check count and match their script;
        86 assert none, 1 marked historical)

**The BLOCKED row's claim was re-derived rather than repeated.** This record's
one incomplete result rests on "nothing serves kind 11266 in this tree". That
sentence had been carried forward across sessions, and carried-forward is not
the same as verified. A distinction worth keeping explicitly, because in a week
neither reading is recoverable from the prose.

Counted, from `src/modules/process-contracts.json`: the `postgres` component
declares `principal_ref` 28 and exactly one stage, `postgres-health` at stage 1,
kind 11265. No component in the file declares kind 11266 at all. The only
mentions anywhere are the constants in `server-go/modules/aimee/store_client.go`
that name the stage the client will one day call, the caller's expectation, not
a server.

So the claim holds, and it now holds because it was counted here rather than
because it was true when someone wrote it down.

## What the commit itself found

Committing was expected to clear the one remaining red, `docs-gen-check`, which
compares generated output against git and so could not pass on an uncommitted
tree. It did. **And `make lint` stayed red, on a different target.**

`check-docs` had never seen `docs/modules/db3.md`. It enumerates with
`git ls-files *.md`, which lists TRACKED files only, so the file was invisible
for as long as it was new, and it reached its first commit carrying ten em
dashes the voice check exists to refuse.

The script already knew about this hole and had patched it by hand:

    # Include intentional new guides before their first commit.
    for relative_path in (
        Path("docs/KB_FLEET.md"),
        Path("docs/WRITING.md"),
        Path("docs/runbooks/change-embedder.md"),
    ):

Three names, maintained by hand, with the failure mode of every enumerated gate.
`db3.md` was the fourth such guide and nobody added it, which is what enumerated gates do, rather
than a lapse.

Replaced with `git ls-files --others --exclude-standard *.md`, so every new
guide is covered by existing code. `--exclude-standard` keeps `.gitignore`'d
paths out, so scratch notes and build output do not become lint failures.
Mutation-verified in both directions: an untracked violating file is now caught,
and a gitignored violating file is still ignored.

The em dashes are fixed and `make lint` is green end to end.

**The general point is about the shape of the evidence.** Every green reading
before this commit was true, and none of them covered this file, because the
gate's own scope excluded it and said nothing about the exclusion. "Everything
passes" and "everything was checked" are different claims, and only the second
one is worth anything, which is the same lesson as the record guard vouching
for one file out of eighty-eight, arriving from the opposite direction.

## test_guardrails restored, and what restoring it uncovered

673 assertions about guardrail classification, deleted for seven lines: the
`db1_init`/`db1_shutdown` fixture pair, and five `db1_session_state_delete`
calls that are teardown between cases.

The store dependency is cut at the call, in `tests/support/session_state_stub.c`,
and **two of its five functions do real work** because two tests genuinely
depend on a round trip:

- Session state saves and loads through one in-memory slot. A load that always misses is a store
that forgets,
  which is a behaviour, and code under test may read back what it just wrote.
- Guardrail events are **counted** by `final_action`, because
  `test_semantic_advisory_pre_tool_check` takes a baseline, drives two tool
  checks and asserts `prompt == before + 2` with the other three buckets
  unchanged. That assertion's subject is the guardrail deciding *prompt* twice;
  the store is only the medium it is read through. A no-op insert with a zeroed
  count would fail the assertion outright, and a fixed count would pass it while
  measuring nothing.

The delete and the ownership upsert return success and do nothing. Their
results are never examined. That is the opposite choice from
`wfe_store_stub.c`, whose functions return failure, and the difference is
whether a future test might start *reading* them.

### `make unit-tests` had been green on binaries of deleted code

Registering the restored test surfaced three failures that had nothing to do
with it: `unit-test-bus-memory-recall`, `unit-test-bus-memory-upsert` and
`unit-test-bus-guardrail-durability` reported `not found`. Their sources were
deleted with the C store and their `TEST_TARGETS` entries were not, so the
runner was trying to execute binaries nothing builds.

It had been passing because the **stale binaries from before the deletion were
still on disk**. A version bump cleaned the object tree and the gap appeared.
Every earlier green reading of `make unit-tests` in this work was, for these
three, a reading of artifacts of code that no longer existed.

`check_tests_are_run` cannot catch this: it verifies that every test SOURCE has
a target, which is the other direction. A target naming a source that is gone is
outside its question.

Retiring them is correct. Each drove the real bus and then read the SQLite
table the consumer wrote to, and that sink does not exist. But the property they
guarded does: every guardrail event the bus accepts reaches the store exactly
once, and a graceful stop drains those in flight. Nothing tests that against the
Go sink. Recorded at the retirement in `src/tests/Rules.mk` as a gap rather than
a decision.

They had been invisible twice, in opposite directions: first listed in an
order-only dependency list so they built every run and executed never, then
executed every run and built never.

### Two edits that made the same mistake as the guards

Removing those entries by regex on the bare target names clipped one out of the
*prose* above the block (`` so `make ` matched a rule ``) which is precisely
the defect this work spent the night removing from its guards, reproduced by the
script doing the removing. The second attempt matched the `.PHONY` pair exactly
and left its recipe line orphaned, which make rejects outright.

Both are one error: **a target is not its name.** It is a comment, an object
rule, a `.PHONY`, a target line and a recipe, and editing by the part you can
grep for leaves the rest.

## test_mcp_git restored

675 assertions about MCP git behaviour, chdir resolution, commit identity
masking, sensitive-file skipping, push/branch/fetch semantics, mirror-workspace
write refusal. Deleted for five lines: an `<sqlite3.h>` include it never used, a
`db_schema.h` include it never used, the `db1_init`/`db1_shutdown` fixture pair,
and two ownership reads.

`git_ownership_stub.c` keeps a real per-`(repo, branch)` map, and that is not
incidental. The two reads are a genuine round trip:

    claim "some-branch" as session-C in repo A
    claim "some-branch" as session-D in repo B
    assert A reports session-C and B reports session-D

The subject is an **MCP git** property. A claim is scoped to its repository,
not to the branch name, and the store is only where the answer is read back
from. A stub keyed on branch alone answers one owner for both, and would retire
the only assertion that distinguishes them while leaving it in the file.

Verified by making exactly that mistake: keying `find()` on branch alone aborts
the test. The restored assertions are sensitive to the property they name.

Two smaller decisions in the same stub, both about not inventing answers:
`find_session_by_prefix` reports **not-found for an ambiguous prefix** rather
than returning the first matching row, so a resolution test cannot pass on row
order; and the feature branch a session's PRs target is a separate table from
ownership, because they are different facts and a test may set one without the
other.

The stubs were split so each symbol has one home, session state, guardrail
events and git ownership in three files rather than one, since the guardrails
and mcp-git binaries need overlapping but different subsets.

## test_agent restored, and a fixture that said yes to the wrong question

826 assertions, plus 103 in `test_agent_delegate_root.c` which shares the
binary. This one could not be done with stubs: the binary links a large slice of
the server and needed **41** store symbols, most of them irrelevant to anything
it asserts. It links the real `$(DB1_CLIENT_OBJS)` instead, which satisfies all
41 with production code that fails closed when no module is attached.

Three tests genuinely read the store back, so they are gated on
`store_module_fixture`, the convention seven suites in this tree already use:
bring the real module up, or skip saying why.

### The fixture aborted the moment a database was named

Standing up PostgreSQL 17.11 and pointing `AIMEE_STORE_URL` at it did not run
the gated tests, it **aborted the whole suite**:

    store_module_fixture: module exited before it attached
                          (check AIMEE_STORE_URL reaches the database)

The DSN was fine. `store_module_fixture_available()` answered on the DSN alone,
and the DSN was never sufficient: the store module does not open PostgreSQL
itself. `storeBackend()` in `cmd/aimee-module` connects as principal 68 and
reaches the database **through the postgres module's SQL stage, kind 11266**.
The one gap this whole document records. Nothing serves it, so the module exits
before it attaches whatever the DSN names.

That combination is the worst arrangement of the two halves. `available()` is
generous, `start()` aborts by design, correctly, since a failure after a yes is
a real fault, and the error message points at the operator's database. Seven
existing suites carried this, not just the restored one, and every one of them
would have died the same way.

**It was invisible because the skip was the normal path.** With `AIMEE_STORE_URL`
unset, all seven skip and the suite is green; a skip that would abort if taken
looks exactly like a skip that would pass. Only setting the variable and running
tells them apart, and nothing in the tree had done that since the store moved.

`available()` now reports the real blocker and returns 0, naming kind 11266 and
saying to delete the block when the stage lands. The DSN check stays as the
second condition.

### And an ordering fault underneath it

Fixed on the way: the fixture configures the module runtime and starts the bus,
and `obs_bus` refuses configuration once running. Several `test_agent` cases
bring the bus up as a side effect, so a fixture started at the first store-backed
test found it already up and died at `configure the module endpoint`. It starts
from `main` now, before anything else. Also only visible with a real database.
The two faults were stacked, and the first hid the second.

### What the restoration cost

`check_tests_are_run` then failed correctly: `test_agent_caps.c` and
`test_agent_responses.c` were in `UNBUILT_SOURCES`, orphaned when `test_agent`
was deleted, and are built again now. Its stale-entry rule caught that without
being asked, the one guard in this sweep that needed no repair.

## test_memory_advanced restored: all four back

513 assertions, deleted for four lines: the `db1_init`/`db1_shutdown` fixture
pair and two cognify enqueues.

Three blocks here are genuinely store-backed and are **gated on
`db1_store_ready()` rather than stubbed**, because what they assert *is* store
behaviour:

- the cognify queue's UNIQUE constraint. A duplicate enqueue leaves `pending`
  unchanged;
- `memory_cognify_drain` reading that queue;
- `memory_maintenance_run`'s idle guard, which records its own last-run time and
  reads it back, so with no store every run looks like the first and
  `s2.skipped` comes back 0.

A stub would have to reimplement each of those constraints to be worth anything,
and then the test would be checking the stub. The postgres family suites already
exercise all three against a real database. This binary links `module_bus_stub`,
whose honest default is "no module attached", so the gate is closed here and the
other ~500 assertions run.

Inverting the assertions to match a storeless run would have been the wrong
repair in the same way editing a validation count is: it encodes "no store" as
the expected shape.

### The four, finished

| file | assertions | how the store dependency was cut |
|---|---|---|
| `test_guardrails.c` | 673 | stubs, two doing real work (state round trip, event counting) |
| `test_mcp_git.c` | 675 | stub with a real per-`(repo, branch)` map |
| `test_agent.c` (+ delegate_root) | 826 + 103 | real `$(DB1_CLIENT_OBJS)`; 3 tests gated on the fixture |
| `test_memory_advanced.c` | 513 | `module_bus_stub`; 3 blocks gated on `db1_store_ready()` |

**2790 assertions** back in the tree, none of them about storage, all of them
deleted because a scan for `db1_|sqlite3|db1/` matched the fixture lines that
let them run.

Four different answers to the same question, and the differences are the point.
A stub is right when the store is scaffolding; a *behaving* stub is right when
the test reads back what it wrote and the subject is the writer; the real client
is right when the symbol surface is too large to fake; and a gate is right when
the assertion's subject is the store itself. Choosing one by habit would have
produced a vacuous test in at least three of the four.

---

# The gap is closed: the store serves

Everything above this line was written while the store module could not run. It
attached, built its client, found nothing serving kind 11266, and exited. Every
green reading in this document before this point was a reading of parts.

## What was missing

`storeBackend()` in `cmd/aimee-module` connects as principal 68 and reaches
PostgreSQL **through the postgres module's SQL stage**. That stage did not
exist. `server-go/modules/postgres/sql.go` is it: seven operations (exec, query,
begin, commit, rollback, migrate, current_version), a transaction registry with
a ceiling and an idle reaper, the limits enforced rather than trusted, and
PostgreSQL's own SQLSTATE carried back so the client can tell a replay from an
outage.

Declared alongside it, in the same change, exactly as `store_client.go` said it
must be: `postgres` gains stage 2 in `process-contracts.json`, and
`aimee-postgres` joins its clients at ref 68 requesting 11266 and serving
nothing.

**`postgres` had to move to `server` as well.** It was declared `kb`-only while
the store module runs on `server`, so the store would have had no postgres
module to call even with the stage written.

## Every store opcode was 1

The client's operation constants were a `const` block with no `iota`:

    opStoreExec uint32 = 1
    opStoreQuery      // ← also 1
    opStoreBegin      // ← also 1
    ...

All seven were 1. Every operation aimee asked of the store, query, begin,
commit, rollback, migrate, current_version, went onto the wire as EXEC.

Nothing caught it because nothing served the other end. Every call failed at the
transport before its opcode was read. The Go compiler found it the moment the
stage existed, refusing a switch with seven identical cases. **A wire constant is
only checked by the far side reading it**, and until there is a far side, "the
numbers are obviously right" is the only check there is.

Pinned now by `store_opcodes_test.go`, to the numbers rather than to being
distinct, asserting distinctness would pass on any renumbering, and renumbering
a live wire silently reinterprets every frame.

## It works

Against PostgreSQL 17.11, on a database created UTF8 with `TEMPLATE template0`:

    93 tables, ledger db1 v21 (21 rows)

All 21 migrations applied through MIGRATE and recorded in the version ledger,
which the module owns and recomputes rather than trusting the client's checksum.

Every suite gated on the store fixture, **zero skips**:

    unit-test-trajectory-batch                 exit=0
    unit-test-server-mgmt-status               exit=0
    unit-test-server-mgmt-checkpoint-client    exit=0
    unit-test-server-write-tier-db1            exit=0
    unit-test-web-search-fuse                  exit=0
    unit-test-curiosity                        exit=0

The fixture now starts the postgres module **beside** the store, with its own
grant and the store's separate outbound grant at ref 68. A serving grant and a
requesting grant are two different admissions, and the store needs both.

## Three defects the working store exposed

Making it run is what found these. None could fail while every call failed first.

**`server_mgmt_status_init()` was deleted from a test with the dead code around
it.** The migration removed `db1_init(path)`/`db1_shutdown()` (correct) and
took the init call with them, which was not dead: it calls
`db1_mgmt_nonce_clear()`. Two assertions were about it, and one is the subject of
a real property: across a restart the high-water mark survives and an in-flight
nonce does not. Without the init the nonce survived, the assertion could not
hold, and the pair stopped testing restarts at all. Restored.

**`ErrStoreUnavailable` discarded the transport's error at all three sites.** A
refused grant, an unreachable socket and a genuinely absent module all produced
the same sentence, naming none of them, which is what I spent the first hour of
diagnosis inside. Wrapped now, so `errors.Is` still answers and the cause travels.

**`test_bus_shutdown_race` could no longer make its second claim.** It asserted
that every event counted written is in the store, read back *after*
`obs_bus_stop()`. That was free with in-process SQLite. It is impossible now: the
store is reached over the bus, and stopping the bus is the thing under test.
Restarting the host does not recover it. The module does not re-attach, measured
with a ten-second poll rather than assumed. Pointing its sink at the real store
instead made it a load test against a database on another host and it stopped
finishing.

It has a counting sink now, which is what the in-process SQLite one actually gave
it: somewhere fast for events to go. That makes the accounting claim **exact**
rather than inferred. The sink reports what it was handed. 54,241 events raced
the stop, all accounted for. The store-durability property is stated in the file
as belonging elsewhere, with the retired `test_bus_guardrail_durability`.

---

# Run two: the whole thing, working

The run at the top of this document was taken while the store module could not
attach. This one was taken after the SQL stage landed, on a **fresh container**
built for it, with `aimee-server` and `aimee-kb` both running and the full fleet
under the supervisor.

    == the module fleet ==
            module processes: 17
    PASS  the supervisor started modules (17)
    PASS  the store module is running

    == the store schema ==
            store tables: 93
    PASS  the store module created its schema (93)
            recorded migrations: 21
    PASS  migrations were recorded (21)

    PASS=11 FAIL=0

Exploratory checks on the same live system: 21 rows in the ledger under owner
`db1` at version 21, and **21 distinct checksums**, one recorded per version
rather than one repeated, which is what proves each migration was hashed as
applied rather than copied from the first.

## The store wire was exercised, not just the schema

Applying the schema proves MIGRATE and CURRENT_VERSION. Those are two of seven
operations, and they take their own path in the stage, `h.migrate` and
`h.currentVersion` use the pool directly. **EXEC and QUERY go through
`h.statement`, which none of that touches**, and a broken statement path would
let the schema apply and fail everything afterwards.

Guessing which HTTP endpoint reads which table was going nowhere, so the
database was measured instead. Driving the daemon moved
`pg_stat_database.xact_commit` from 163 to 168 and `tup_returned` by 242, the
store module held **two connections as `root`** (its pool) and
`pg_stat_activity` showed a real family query mid-flight:

    SELECT id, schedule, mode, script, prompt, workdir, context_from, ...

That is `opStoreQuery` crossing the wire and returning rows.

## Four things the run found, in order

**The supervisor gives no ordering guarantee.** It starts every module in the
manifest back to back and each registers asynchronously, so the store made its
first call before postgres had claimed 11266 and exited:

    [module-supervisor:server] starting postgres
    store: schema: read the applied schema version: ... capability absent
    aimee-module: module "aimee" could not start

Fixed with a bounded wait, thirty attempts, one second apart, retrying only
`ErrStoreUnavailable`. Ordering the manifest would not fix it: registration is
asynchronous, so starting postgres first narrows the window, and a window that
closes on a fast machine reopens on a loaded one.

**The first version of that wait ran zero times.** It wrapped `storeBackend`,
which talks to nothing, it opens a bus client and constructs a `Store`.
Construction succeeded and the failure was one line later in `ApplySchema`, the
first call that leaves the process. The run failed identically, which is how it
was caught: *a fix that changes nothing looks exactly like a fix that was not
deployed*, and this work had just been bitten by the second twice.

**The rig never created the role the daemon connects as.** The DSN carries no
user, so libpq uses the process's OS name, `root`, since that is what the
supervisor runs modules as, and PostgreSQL answered SQLSTATE 28000, `role
"root" does not exist`. It could not have come up before: the C store was a
SQLite file with no roles at all. Added to `e2e-252-dbsetup.sh`.

That error is also the first proof the stage worked. A real SQLSTATE and a real
message came back through the reply frame, which is the whole point of carrying
them rather than collapsing everything into "failed".

**`install-modules` only creates missing names.** Redeploying a rebuilt binary
left all seventeen module names on the old one and reported
`0 module name(s) installed / names present: 18`, which reads like success. The
third instance of this trap in this work, and the reason the checksum of the
running binary is now compared against the local one rather than assumed.

## What is still not covered

`unit-test-bus-guardrail-durability`'s property: every guardrail event the bus
accepts reaches the store exactly once, and a graceful stop drains those in
flight. It needs a test against the Go sink that does not depend on the bus it
is verifying, and `test_bus_shutdown_race` cannot host it for the same reason.
Recorded here and at both sites rather than left to be rediscovered.

---

# The guardrail durability property, and what writing its test found

The gap this document has carried since the retirement. *Every guardrail event
the bus accepts reaches the store exactly once, and a graceful stop drains those
in flight*, now has a test. Writing it found three defects, two fixed and one
open.

## The test is in two halves, and has to be

`test_bus_guardrail_durability.c` emits N events with unique identities, stops
the bus, and reports what the bus counted. **It cannot check its own work**:
stopping the bus is half the property, and the store is reached *over* that bus,
so after the stop there is no transport to ask through. Restarting the host does
not recover it. The module does not re-attach, measured with a ten-second poll.

`test_bus_guardrail_durability.sh` does the read-back straight out of
PostgreSQL, after the emitter has exited and its modules with it. It checks the
count against what the bus accepted, that no identity appears twice (a loss and
a duplicate netting to the same total passes a count and fails this), and that
`overall_risk`, `final_action` and `dry_run` round-tripped as double, text and
boolean.

## Fixed: `obs_bus_module_available()` deadlocked shutdown

It took `start_lock`. `obs_bus_stop()` holds `start_lock` while waiting for
callers to drain and joining the consumer, and the consumer, inside
`persist_guardrail`, asks whether the kind is served *first*. Both threads
parked in futex waits, forever.

Impossible while the store was in-process SQLite: that sink made no bus call, so
the probe was never on its path. `obs_bus_adapter.c` still says the sink is
"safe to call from the bus consumer thread", which was true when written and
became false without either file being edited.

Now registers in `module_callers` and reads the atomic instead, the discipline
`obs_bus_module_call` already used, for the same reason.

## Fixed: the final drain could not reach the store

`obs_bus_stop()` cleared `accepting_calls` and stopped the runtime listener
*before* joining the consumer. So the drain processed every queued event and
every sink call was refused: `emitted 3, written 0, dropped 3`. A shutdown
discarded whatever was still queued.

Both moved after the join. The drain now runs with the call path open, which is
what "a graceful stop drains those in flight" requires.

## FIXED (was OPEN): a store call from the bus consumer thread did not complete

With both fixes in, the events still do not arrive, `DEADLINE_EXCEEDED`, and
**zero rows reach PostgreSQL**. It is not the stop: setting a settle window puts
the failure squarely inside it, with the bus fully running.

The contrast that locates it: every store-backed suite listed above passes
against the same PostgreSQL, and `unit-test-server-mgmt-status` does a
*transactional* nonce consume. Those call the store from the **main** thread.
This calls it from the obs_bus **consumer** thread, which is what routes peer
traffic, so while it blocks waiting for the store's reply, the store's own call
to the postgres module has nobody to route it.

That is architectural: the guardrail sink cannot make a blocking module call
from the thread that routes the bus. **See the next section: it is fixed.**

**The test is committed and registered.** Without `AIMEE_STORE_URL` it skips, so
`make unit-tests` is green; with one it fails, which is the truth. That is the
opposite of the state this property was in before, a retired test and a note in
a Makefile, and it is why the defect is now a failing assertion rather than a
paragraph.

---

# The guardrail write, moved off the routing thread

The open item above is closed. It was mine to close: making the store a bus
client is what turned an in-process write into a blocking call from the thread
that routes the bus.

## Confirmed in one process, not inferred from two

The earlier diagnosis compared test binaries. This settles it inside a single
process, where the bus, modules, database and grants are identical between the
two calls:

    main-thread store call: rc=0        <- the store answers this thread
    consumer-thread calls:  result 4    <- DEADLINE_EXCEEDED, 0 written, 3 dropped

And the cause is in the consumer loop itself:

    bus_host_pump(&g.host);   // routes peer traffic, including replies
    uint32_t n = drain();     // calls the sinks

`drain()` waits inside the very thread that would deliver its reply. It resolves
only when the 2s deadline expires. **Blast radius is one sink**: the audit sink
calls `audit_action_log`, which writes the WORM ledger (a file) and makes no
module call. Checked rather than assumed.

## The fix, and why the ordering is forced

A writer thread owns the guardrail sink. The consumer copies each payload into a
bounded queue and keeps pumping; the writer makes the call and counts the event
**written only when the sink reports it durable**, so `obs_bus_written()` still
means "in the store" and the exactly-once claim is unchanged. A full queue drops
and counts it, blocking would put the consumer back to waiting on the store,
which is the defect being removed.

Shutdown is the subtle part, and both obvious orders are wrong:

- finish the writer **before** the consumer's final drain, and the events that
  drain rescues meet a stopped writer, `written 0, dropped 3`;
- finish it **after** the consumer is joined, and every queued call times out
  with nobody routing replies, also `written 0`.

Both were measured, not reasoned about. The order that works has three steps
and a wait between each: signal the drain, **wait for the drain to complete**,
finish the writer against a still-pumping consumer, then release the consumer.
The middle wait is what the first attempt lacked, and its absence looked exactly
like the original defect.

## Proven

    emitted 2000, written 2000, dropped 0

    PASS  the store gained exactly 2000 rows, matching what the bus wrote
    PASS  no identity appears twice
    PASS  overall_risk round-tripped as a double on every row
    PASS  final_action round-tripped as text on every row
    PASS  dry_run round-tripped as a boolean on every row

The identity check was verified non-vacuous by planting a duplicate row for `s7`
and watching it report one. A loss and a duplicate that net to 2000 pass the
count and fail there, which is the whole reason each event carries an identity.

All seven store-backed suites pass against a clean PostgreSQL after the change,
including `unit-test-bus-shutdown-race`, which exercises `obs_bus_stop()` under
concurrent producers.

## One thing the run turned up that is not a code defect

Six suites failed against the database used earlier in this work with
`migrate db1 v20 ... constraint already exists`. That database was populated
while the migration ledger was still called `store_schema_version`; renaming it
to `schema_migrations` left the schema applied and its history invisible, so the
module replayed from version 1. A pre-rename database that never shipped. On a
clean database all twenty-one apply and all seven suites pass.

---

# Run three: the whole system, with the threading change in it

Run two proved the store on hardware. It was taken **before** the obs_bus
threading change, so the daemons had never run with the guardrail writer thread,
and that change alters shutdown ordering in a process every module attaches
to. A green unit suite says little about it: no unit test stops a real
`aimee-server` with a real fleet attached.

Fresh container, current build, both daemons.

    == the module fleet ==
            module processes: 17
    PASS  the supervisor started modules (17)
    PASS  the store module is running
    PASS  the store module created its schema (93)
    PASS  migrations were recorded (21)

    PASS=11 FAIL=0

## Shutdown, which is what actually changed

`obs_bus_stop()` now waits for the consumer's final drain, finishes a writer
thread against a still-pumping consumer, then releases the consumer, three
waits where there were none, and a mistake in any of them is a daemon that never
exits.

    PASS  aimee-server exited 2s after SIGTERM
    PASS  no aimee-server left running
    PASS  aimee-kb exited 5s after SIGTERM
    PASS  no aimee-kb left running

Both daemons, both clean.

## Durability, on the same container as the fleet

The property had been proven from a workstation against a remote database. Run
here, on the e2e container, against the store the fleet had just used, with the
binaries that were deployed:

    emitted 2000, written 2000, dropped 0
    PASS  the store gained exactly 2000 rows, matching what the bus wrote
    PASS  no identity appears twice
    PASS  overall_risk round-tripped as a double on every row
    PASS  final_action round-tripped as text on every row
    PASS  dry_run round-tripped as a boolean on every row

## What this rig cannot show, stated rather than implied

**The daemon emitted no guardrail events of its own.** `/v1/hooks/pre` answers
and blocks, `"BLOCKED: write blocked because this session is not running in a
worktree"`, but that refusal happens at the worktree guard, upstream of the
semantic orchestrator that calls `gsem_record`. Reaching it needs a
worktree-backed agent session, which this rig does not create.

So the daemon-side evidence is: it starts, it serves, and **it stops cleanly
with the new ordering**. The write-and-drain path itself is evidenced at 2000
events through the same obs_bus code, on the same container, rather than through
the daemon's own orchestrator. That distinction is worth keeping: it is the
difference between "the code path works here" and "the daemon exercised it".
