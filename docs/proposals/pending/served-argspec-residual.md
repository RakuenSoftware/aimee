# Proposal: the 21 CLI methods the served argument spec cannot describe

- **State:** PENDING — no slice started.
- **Residual of:** the served thin-client argument specs, PR #2821.
- **Date:** 2026-08-20.
- **Charter roles:** Constrain-Verify / Gate-Promote.
- **Scope:** the 21 of 181 CLI-reachable methods that still build their request body from a
  compiled marshaller rather than a served spec.

## Why this exists

PR #2821 moved 160 of 181 CLI-reachable methods onto argument specs served from
`GET /v1/cli/manifest`, so a command absent from an installed client runs end to end against a
newer server. The remaining 21 are not "not looked at". Each is blocked by something the spec
language deliberately cannot say, and every one of those blocks is a decision that belongs to
whoever owns the CLI contract rather than to the person writing specs.

This file exists so that partial work is linked rather than called complete, and so the next
person changing this area inherits the reasoning instead of re-deriving it.

**Read the header comment in `src/server/cli_argspec_defs_data.h` first.** It carries the line,
the four times that line was drawn wrongly, and what each defect looked like. This proposal is
the forward-looking half; that comment is the record.

## The line the vocabulary holds

> A field's rule may depend on ITS OWN value, its own flags, named client facts the SERVER asked
> for, and the invocation's ARITY. It may not depend on ANOTHER FIELD's value, and it may not
> compute one.

That line moved four times during #2821, each time because it had been drawn on convenience and
then defended as principle:

| claim | why it was wrong | cost |
| --- | --- | --- |
| "43 methods must never be served, they read client state" | conflated the client DECIDING it needs `getcwd()` with SUPPLYING it when a served spec asks | 26 methods |
| "no branch may decide which fields exist" | the vocabulary is full of conditionals; an arity gate reads the invocation's shape, as `max_positionals` already does | 6 |
| "a literal comparison is string surgery" | `skip_if_dash` already tests a field's own value against a prefix | 2 |
| "the `user_capture` family is string surgery" | a constant prefix and a length limit are less computation than the admitted clamp | 4 |

The lesson is not "the line was too strict". It is that a refusal stated as a principle should be
checkable against the code, and these were not. The reasons below are written so they can be
checked.

## Non-goals

This proposal does not propose a policy language, a general expression evaluator, an environment
reader, or a second argv parser in the spec. Each is the thing the line exists to prevent, and each
would make a served spec a program transmitted over the wire.

## Group 1 — six methods whose file CONTENTS go in the body

`delegate`, `delegate.launch`, `roundtable.review`, `skill.create`, `skill.edit`, `vault.unlock`

**Blocked by:** the client must read a file (or vault key material) and put the bytes in the
request.

**Why not a `file_contents` source.** Today the set of commands that read a file is fixed in the
client binary. Served, the SERVER would choose which argv slot becomes a path, so a compromised
server — or anyone able to answer as one — could make `aimee kb search /etc/passwd` read that file
instead of searching for it. That is a capability grant, not an expressiveness gap, and it is
qualitatively different from `cwd` and `session`, which are two fixed facts the client already
sends.

**The intended resolution is the other direction.** The server and the kb already have to stay
current with file contents. The plan is to index and embed EVERY tree rather than main alone, with
pruning for trees that go away. Once the server holds every tree, these commands send a REF — a
path, a tree id — and the server reads its own copy. The client stops carrying bytes at all: a
smaller client than `file_contents` would give, and the exfiltration path is removed rather than
accepted.

**Therefore: do not add `file_contents`.** Serving these the risky way now would be work thrown
away plus a capability the real design never needs.

**Dependency:** whole-tree indexing and pruning. **Open question this proposal does not answer:**
who owns pruning. Indexing every tree means the index grows with every worktree ever created, and
an index that is never reaped fails slowly and quietly — the same shape as the defects #2821 was
about.

## Group 2 — seven methods that parse argv themselves

`git.cli`, `git.verify`, `index.ast_grep`, `tool.call`, `memory.supersede`, `skill.autostub`,
`skill.lifecycle`

**Blocked by:** they never call `cli_args_parse`. They loop over argv with a grammar of their own —
`key=value` pairs, in-place string mutation, a `--status` special case, refusal on a missing `=`,
and inline-only `--flag=value` in `memory.supersede`'s case.

**The observable difference is small and real:** they take `--snapshot X` but NOT `--snapshot=X`,
which `cli_args_parse` accepts and therefore every served spec does. A spec naming those flags would
agree on one form and diverge on the other. The suite now carries `--flag=value` samples precisely
so that divergence cannot ship unnoticed.

**What would unblock them:** convert them to `cli_args_parse` like every other marshaller. That is
a marshaller change, and it WIDENS accepted syntax — `--snapshot=X` starts working. That is a
change to a shipped CLI's accepted input, which is why it is not made here.

**Acceptance if attempted:** the differential test must pass with the existing `--flag=value`
samples, and the change should be announced as an accepted-syntax widening rather than a refactor.

## Group 3 — seven methods that need a cross-field rule

| method | the rule |
| --- | --- |
| `cron.enable`, `cron.disable` | send `job_id` OR `all: true`, and refuse when NEITHER is given |
| `trigger.fire` | `--source` AND (`--task` OR `--proposal`) |
| `primary.set` | `--show` / `--clear` / a positional select three different METHODS, not three fields |
| `delegate.status` | `job_ids` (array) or `job_id` (scalar), by argument count |
| `catalog.show` | splits `provider:model` on a colon and truncates at 64 bytes |
| `index.span` | the positional INDEX depends on whether an earlier argument parses as a number |

**Blocked by:** each consults another field's value, or computes one. This is the half of the line
that still forbids something, and it holds where it should — `skill.archive` is refused for the
same reason: it gates a field read from `argv[2]` on `argv[1]` matching a literal.

**What would unblock them:** a marshaller change that removes the cross-field dependency. For
`cron.enable` that means sending both fields and letting the server reject the empty case, or
splitting the command. Either alters what the CLI sends, so the server contract moves with it.

**Do not** solve this by adding conditional presence keyed on another field. That is the point at
which a spec stops being data.

## Group 4 — one cost decision

`get_help`

**Blocked by:** nothing in principle. It is pure argv with no client state and no branch. It wears
the MCP tool-call envelope — method `help.get`, a constant `tool`, a nested `arguments`, and no
`protocol_version` — and serving it needs four mechanisms for one method, one of which is envelope
suppression.

**Why that one mechanism is the objection:** every served spec currently produces a properly
enveloped request. An escape hatch used exactly once is a poor trade for that invariant.

**Revisit when** a second method wants the same shape. The other four MCP-shaped methods do not
count: they are in Group 2 on their own merits.

## Slices, if this is taken up

1. **Whole-tree indexing and pruning** (unblocks Group 1). Largest, and wanted for its own sake.
2. **`cli_args_parse` conversion** (unblocks Group 2). Independent per method; each lands with its
   own differential samples and is announced as a syntax widening.
3. **Cross-field removal** (Group 3), per method, each a CLI contract change.
4. **`get_help`** (Group 4), only alongside a second method needing the envelope.

No slice depends on another. Any one can be taken without the rest.

## Acceptance checks

Any slice that serves a method must:

- add the spec and its samples to `test_cli_argspec`, which runs the REAL compiled marshaller
  against the spec interpreter over identical argv;
- sample every arity the spec implies, from one positional to n+1 — the gap that let
  `index.structure` ship a spec sending the file as a `project`;
- sample the `--flag=value` form;
- sample the awkward input for whatever convention the method uses, including any limit or refusal,
  because a rule the spec omits is invisible to a spec-derived sample set;
- keep `scripts/check_argspec_numeric_parity.py` passing, which compares each spec's numeric type
  against the parse its marshaller calls;
- be plant-tested: introduce the violation and confirm the check FAILS on it. A check that has
  never failed is decoration.

Run `make -C src verify-ci`, not `verify-local`: the latter omits `build-integrity` and
`integration-tests` and can bless a branch CI rejects.

## Status

Residual of PR #2821, which served 160 of 181. No slice here is started. Nothing in this file blocks that
PR; it records what that PR deliberately did not do, and why, so the next change to this area
argues with a stated reason rather than rediscovering it.
